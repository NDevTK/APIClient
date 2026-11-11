/* safe-fetch.js — the single external-fetch entry point for the analyzer.
   Every request the analyzer makes off the network (documents, lazy chunks, source maps, discovery probes)
   goes through `safeFetch`, so its security invariants live in one auditable place:
     • cookies omitted by default; in credentialed mode the reply passes this file's own SOP/CORS, because a
       host-permission fetch bypasses the browser's;
     • a non-GET only as the page's own act on its unforced run, never credentialed, never by a grant
       (the `method` row);
     • a destructive-path deny list on credentialed requests not graded `observed` (`_destructiveToken`);
     • http(s) only and no userinfo, so a crafted URL cannot read local or extension resources;
     • origin-relative SSRF on the initial and the landed URL: a public page may not reach the person's private
       network through the extension's host permissions, while a private page may reach its own;
     • CORB by the request's destination, for every code loader at once;
     • the per-origin egress policy (`_firingRefusal`), which decides whether an act may be spent at all.
   It runs in the offscreen document (`ast-worker.html`, after `check.js`, which defines the asserts) and
   verbatim in `engine/trusted.mjs`'s vm realm; each host's wire (`_wire`, below) is the only fetch it spends. */
/* Returns a plain record, not a Response, so both hosts read one shape: { ok, status, statusText, headers
   (lowercased), body, urlList, computedType, refusal }.
   The body is bytes. A decode is a semantic the engine owns: HTML §8.1.4.2's "fetch a classic script" decodes
   "using encoding as the fallback encoding", with the `Content-Type` charset and a BOM deciding, so the engine
   must receive what the server sent. No security decision reads a decoded body; CORB's sniff decodes only for
   its own comparison.
   `urlList` is Fetch §2.2.6's response URL list, which only this zone can report because the redirect chain
   exists only here; it is [requested] or [requested, landed] (see `_exposedUrlList`).
   Type sniffing lives here and nowhere else in the extension. What a flow needs mid-execution belongs in the
   engine; this is a decision the trusted zone makes once about a reply it fetched, and `computedType` states
   it to the renderer, so no zone downstream sniffs or re-parses a raw header for a type of its own. */
/* This file holds its realm's network. It takes the platform's `fetch` at load, and leaves in its place, and
   in place of every other network constructor the realm has, a function that crashes, so a request from any
   other script in the zone fails at its call rather than skipping the checks above. The one read let through
   is of this extension's own package (renderer-host.js builds each renderer from package files), which
   reaches no other party. In an extension document every send leaves through lib/wire.js's `wireSend`, handed
   the platform fetch taken here; a realm given no `fetch` holds nothing, and `_wire` asserts before spending. */
var _wire = (function () {
  var platformFetch = globalThis.fetch;
  var packageOrigin = typeof globalThis.location === "object" && globalThis.location !== null
    ? globalThis.location.origin : null;
  function seal(name, value) {
    Object.defineProperty(globalThis, name, { value: value, writable: false, configurable: false });
  }
  seal("fetch", function fetch(input, init) {
    CHECK(packageOrigin !== null && typeof input === "string" &&
          new URL(input, globalThis.location.href).href.indexOf(packageOrigin + "/") === 0,
          "a script in the trusted zone called fetch(" + JSON.stringify(String(input)) + ") — the only network " +
          "in this realm is safeFetch's, and the only read outside it is of this extension's own package");
    return platformFetch.call(globalThis, input, init);
  });
  ["XMLHttpRequest", "WebSocket", "EventSource", "WebTransport"].forEach(function (name) {
    if (name in globalThis) seal(name, function () {
      CHECK_FAIL("a script in the trusted zone constructed " + name + " — the only network in this realm is " +
                 "safeFetch's");
    });
  });
  if (typeof globalThis.navigator === "object" && typeof globalThis.navigator.sendBeacon === "function")
    Object.defineProperty(globalThis.navigator, "sendBeacon", { writable: false, configurable: false,
      value: function () { CHECK_FAIL("a script in the trusted zone called sendBeacon — the only network in " +
                                      "this realm is safeFetch's"); } });
  /* In an extension document the wire is lib/wire.js, which reads webRequest's record of each send; a process
     host (engine/trusted.mjs) hands this realm its own fetch, whose Response states its 3xx and Set-Cookie. */
  var extensionDocument = typeof chrome === "object" && chrome !== null && !!chrome.runtime;
  function sent(href, init) {
    if (extensionDocument) {
      CHECK(typeof wireSend === "function" && typeof platformFetch === "function",
            "safeFetch spent a request in an extension document that loaded no lib/wire.js, or was given no fetch");
      return wireSend(platformFetch, href, init).then(function (a) {
        _wireSetCookies.set(a.response, a.setCookie);
        return a.response;
      });
    }
    CHECK(typeof platformFetch === "function",
          "safeFetch spent a request in a realm that was given no fetch — a policy-only host loads this file " +
          "to read its decisions, and reaching the wire there means a decision it asked about fired");
    return platformFetch.call(globalThis, href, init);
  }
  /* Both wires reject a network error with a TypeError (Fetch §5.6); each is recorded, so the chokepoint
     tells the network's answer from a defect of its own. By name: a process host's fetch is another realm's. */
  return function _wire(href, init) {
    return _wireWatched(href, init, function (h, i) {
      return sent(h, i).then(null, function (e) {
        RETHROW_FATAL(e);
        if (e !== null && typeof e === "object" && e.name === "TypeError") _wireNetworkErrors.add(e);
        throw e;
      });
    });
  };
})();
var _wireNetworkErrors = new WeakSet();
/* How long a send's wire may be silent, while this zone waits on it for a head or for more body, before this
   zone stops waiting. Chrome has no such timer: net/http/http_stream_parser.cc and http_network_transaction.cc
   time out nothing once a connection is made. Firefox has one, `network.http.response.timeout` = 300
   (modules/libpref/init/all.js: "Timeout connections if an initial response is not received after 5 mins."),
   which nsHttpConnection::OnSocketReadable switches off at the first readable byte. So no browser fails a body
   that has begun arriving, and the end is graded `decline` (only this tool stops waiting); the value is
   Firefox's, so no response is given up on before a browser would give up on its head. It is the wire's answer
   and never the scheduler's: a reply's due point waits on its wire (lib/reply-schedule.js), and without an end
   a server holding a body open would hold its engine instance for good. */
var WIRE_IDLE_TIMEOUT_MS = 300000;
/* The errors a silent wire ended its send with (_wireWatched), which safeFetch answers as `wire-idle`. */
var _wireIdleErrors = new WeakSet();
/* `send(href, init)` with the wire's idle end: no head within WIRE_IDLE_TIMEOUT_MS rejects with an idle error,
   and a body read pending that long errors the body with one; either aborts the send. The body is re-wrapped so
   each read of the caller's is one read of the wire's. A body read that fails while the caller has not aborted is
   recorded as a network error, as a rejected send is. The caller's `init.signal` still ends the send. */
function _wireWatched(href, init, send) {
  var end = new AbortController(), caller = init.signal, timer = null, idle = null;
  function disarm() { if (timer !== null) { clearTimeout(timer); timer = null; } }
  function arm(onIdle) {
    DCHECK(timer === null, "the wire's idle timer was armed twice for " + href);
    timer = setTimeout(function () {
      timer = null;
      idle = new TypeError("wire: " + href + " sent nothing for " + WIRE_IDLE_TIMEOUT_MS + " ms");
      idle.netError = null;
      _wireIdleErrors.add(idle);
      onIdle(idle);
      end.abort();
    }, WIRE_IDLE_TIMEOUT_MS);
  }
  if (caller) {
    if (caller.aborted) end.abort();
    else caller.addEventListener("abort", function () { disarm(); end.abort(); });
  }
  arm(function () {});
  return send(href, Object.assign({}, init, { signal: end.signal })).then(function (resp) {
    disarm();
    if (idle !== null) {
      if (resp.body !== null) resp.body.cancel().then(null, function (e) { RETHROW_FATAL(e); });
      throw idle;
    }
    if (resp.body === null) return resp;
    var reader = resp.body.getReader();
    var body = new ReadableStream({
      pull: function (c) {
        arm(function (e) { c.error(e); });
        return reader.read().then(function (step) {
          disarm();
          if (idle !== null) return;
          if (step.done) c.close(); else c.enqueue(step.value);
        }, function (e) {
          disarm();
          RETHROW_FATAL(e);
          if (idle !== null) return;
          if (!(caller && caller.aborted) && e !== null && typeof e === "object") _wireNetworkErrors.add(e);
          c.error(e);
        });
      },
      cancel: function (why) { disarm(); return reader.cancel(why); }
    });
    var own = new Response(body, { status: resp.status, statusText: resp.statusText, headers: resp.headers });
    if (_wireSetCookies.has(resp)) _wireSetCookies.set(own, _wireSetCookies.get(resp));
    return own;
  }, function (e) {
    disarm();
    if (idle !== null) throw idle;
    throw e;
  });
}

/* Fetch §2.2.3 "Statuses"' null body statuses, whose Response has no body. */
var _NULL_BODY_STATUSES = [101, 103, 204, 205, 304];
function _b64OfBytes(u8) {
  var bin = "";
  for (var i = 0; i < u8.length; i += 8192) bin += String.fromCharCode.apply(null, u8.subarray(i, i + 8192));
  return btoa(bin);
}
/* The Set-Cookie fields of each Response the wire answered, which the Response itself hides (Fetch §2.2.2
   "Headers"' forbidden response-header names). A process's own fetch states them on its Response. */
var _wireSetCookies = new WeakMap();
function _setCookiesOf(resp) {
  if (_wireSetCookies.has(resp)) return _wireSetCookies.get(resp);
  CHECK(typeof resp.headers.getSetCookie === "function",
        "a reply's Set-Cookie fields were asked of a Response the extension document's wire did not answer, in a " +
        "realm whose Headers has no getSetCookie — its cookies would be stored as none");
  return resp.headers.getSetCookie();
}
/* The identity of a send as the wire carries it: the method, the URL without its fragment, the header list,
   the speak-as set its document's rule writes (Origin, Referer, Cookie and the rest, as composed for this
   send), the body, the request's credentials mode and the HTTP cache partition (httpCachePartitionOf). The
   speak-as set is a set of fields a rule writes, so it is compared by name; the header list in its order. */
function _wireSendKey(href, init, facts) {
  var u = new URL(href);
  u.hash = "";
  var speak = init.speakAs;
  var body = init.body === undefined ? null
    : typeof init.body === "string" ? ["text", init.body] : ["bytes", _b64OfBytes(init.body)];
  return JSON.stringify([init.method, u.href, init.headers === undefined ? [] : init.headers, speak.origin,
                         speak.stated, Object.keys(speak.headers).sort().map(function (n) { return [n, speak.headers[n]]; }),
                         body, facts.credentials, facts.partition]);
}
/* What every `_wireSendKey` of `method` to `href` begins with, whatever else the send carries. */
function _wireSendKeyStart(method, href) {
  var u = new URL(href);
  u.hash = "";
  return JSON.stringify([method, u.href]).slice(0, -1) + ",";
}
/* One send of a page's request (safeFetch's, and its CORS preflight), answered {resp, requestTime,
   responseTime}. A send that speaks as a document is made once per identity (_wireSendKey) within that
   document's exploration, and a byte-identical send of the same document is answered from the record
   (lib/send-coalesce.js): one act of one document is sent once however many forced worlds make it. Its reply
   is a Response of this asker's own, whose Set-Cookie fields ride beside it as the wire's do, so every check,
   cookie store and grade after this runs on it as on a fresh reply. `facts` is {credentials, partition} of the
   send's identity, or null for a send that speaks as no document. `reached` is told, before the wire is asked,
   whether this ask makes the send (true) or is answered from the document's record (false); `madeBy` is what
   admitted this ask (the firing decision's kind), kept on the record of a send it makes.
   Named residual. Not covered: a send that speaks as no document (no `opts.identity`, as engine/trusted.mjs's
   navigation load) is never coalesced, since no document key scopes it. Next diff: that load states its
   issuing document's identity. Its absence shows as a server log counting one navigation of one document
   once per world that makes it. */
async function _wireSend(href, init, facts, reached, madeBy) {
  var speak = init.speakAs === undefined ? null : init.speakAs;
  CHECK((speak === null) === (facts === null) &&
        (facts === null || (!!facts && typeof facts.credentials === "string" && "partition" in facts)),
        "a send speaks as " + (speak === null ? "no document" : speak.docKey) + " and states " +
        (facts === null ? "no" : facts === undefined ? "an absent" : "its") + " identity's credentials mode and " +
        "partition — the two are one identity");
  if (speak === null) {
    reached(true);
    var requestTime = Date.now();
    var bare = await _wire(href, init);
    return { resp: bare, requestTime: requestTime, responseTime: Date.now() };
  }
  var key = _wireSendKey(href, init, facts);
  reached(sendCoalesceMadeBy(speak.docKey, key) === null);
  var answer = await sendCoalesce(speak.docKey, key, init.signal, async function (signal) {
    var resp = await _wire(href, Object.assign({}, init, { signal: signal }));
    var headers = [];
    resp.headers.forEach(function (v, k) { headers.push([k, v]); });
    DCHECK(_NULL_BODY_STATUSES.indexOf(resp.status) < 0 || resp.body === null,
           "the wire answered status " + resp.status + " with a body — Fetch §2.2.3 \"Statuses\" makes it a null " +
           "body status");
    return { head: { status: resp.status, statusText: resp.statusText, headers: headers, setCookie: _setCookiesOf(resp) },
             body: resp.body };
  }, madeBy);
  var own = new Response(answer.body, { status: answer.head.status, statusText: answer.head.statusText,
                                        headers: answer.head.headers });
  _wireSetCookies.set(own, answer.head.setCookie);
  return { resp: own, requestTime: answer.requestTime, responseTime: answer.responseTime };
}

/* A tuple origin (scheme://host[:port]) is usable for same-origin and CORS. An opaque origin reports "null",
   or this zone's minted "null:<uuid>" token, and is unique, so it is same-origin with nothing, not even
   another "null". Only a tuple origin contains "://", so this one test separates them. */
function _isRealOrigin(o) { return typeof o === "string" && o.indexOf("://") > 0; }
/* Whether `u` parses as a URL whose scheme is an HTTP(S) scheme (Fetch §2.1 "URL": "http" or "https"). */
function _isHttpSchemeUrl(u) {
  if (typeof u !== "string" || !URL.canParse(u)) return false;
  var p = new URL(u).protocol;
  return p === "http:" || p === "https:";
}
/* The empty byte sequence every refusal's body is, so a body is one type on every path; a caller telling "no
   bytes" from "bytes I cannot read" reads `ok` and `status`. */
function _NO_BYTES() { return new Uint8Array(0); }
/* What a refusal is, graded by the arm that made it. A host that turns a refusal into Fetch §5.6 "Fetch
   methods"' network error ("reject p with a TypeError") resumes the page down its failure path under a fact
   about the origin, so the grade answers one question: would a real browser performing this request also
   produce a network error?
     "network" — yes, and the network error is the faithful answer: Fetch §4.3 "Scheme fetch" ends "Return a
                 network error", a failed §4.10 "CORS check" makes §4.4 "HTTP fetch" return one, and Local
                 Network Access and CORB refuse their cases. The flow resumes down its failure path correctly.
     "decline" — only this tool refuses, because spending an act on a request no client makes is a policy
                 question. There is no observation to hand back, so the flow parks, which is a search not yet
                 solved and never a verdict, and fires the day the origin is widened.
   A permanent decline (the deny list) needs no third word: a parked flow emits nothing, so the scheduler sinks
   it, and the reason in `statusText` tells the reader which rule holds it. A decline costs the page's own
   `.catch` arm, and the answer to that is a fork, never a fabricated network error. The grade is stamped by
   the arm, so a consumer never re-derives the policy or matches `statusText`. */
/* The header map is stated by the arm: a CORB refusal happens after the reply arrived and its headers are
   part of what was learned, while a pre-request refusal has no response, so `{}` there is a statement. */
var _REFUSAL_KINDS = ["network", "decline"];
function _refused(kind, reason, urlList, headers) {
  DCHECK(_REFUSAL_KINDS.indexOf(kind) >= 0,
         "a refusal was graded `" + kind + "`, which is neither of the two this file states: `network` (a real " +
         "browser refuses this same request, so Fetch §5.6's network error is the faithful answer) and " +
         "`decline` (only this tool refuses, so there is nothing to hand back and the flow parks). A third " +
         "word would be a grade no host has an arm for, and the arm an unknown grade falls to is whichever a " +
         "consumer wrote as its else");
  DCHECK(typeof reason === "string" && reason !== "",
         "a refusal carries no reason — `statusText` is the only account a page or a person ever gets of a " +
         "request this zone did not make, and the empty string is a legitimate reason phrase from any HTTP/2 " +
         "response, so an unwritten one is indistinguishable from a reply");
  DCHECK(Array.isArray(urlList),
         "a refusal carries no Fetch §2.2.6 \"Responses\" URL LIST — the request URL is a fact even when the " +
         "reply is not, and the engine reads `response.url` off this list's last item");
  DCHECK(headers !== null && typeof headers === "object" && !Array.isArray(headers),
         "a refusal carries no header map — every record this file returns has one, and a caller that read " +
         "`undefined` here would be reading it off a refusal instead of off the reply it thought it had");
  return { ok: false, status: 0, statusText: reason, headers: headers, body: _NO_BYTES(),
           urlList: urlList, computedType: "", refusal: { kind: kind, reason: reason } };
}
// HTML's JavaScript MIME type list and Chromium's CORB-protected set: the two tables the CORB rule is stated
// over.
function _jsMime(m) {
  return m === "text/javascript" || m === "application/javascript" ||
    m === "application/ecmascript" || m === "text/ecmascript" ||
    m === "application/x-javascript" || m === "text/x-javascript" ||
    m === "application/x-ecmascript" || m === "text/jscript" ||
    m === "application/node" || /^text\/javascript1\.[0-5]$/.test(m);
}
function _corbProtectedMime(m) {
  return m === "text/html" || m === "text/xml" || m === "application/xml" ||
    /\+xml$/.test(m) || m === "application/json" || /\+json$/.test(m) ||
    /^multipart\//.test(m);
}
// Fetch's determine nosniff, over the `X-Content-Type-Options` value as "get a header" has already joined its
// duplicates: "let values be the result of getting, decoding and splitting the header … if values[0] is an
// ASCII case-insensitive match for `nosniff`, return true." Only the first comma-separated value counts, so
// `foo, nosniff` is sniffable, as it is in a browser.
function _determineNosniff(v) {
  if (typeof v !== "string") return false;   // absent header = §"values is null" = false
  return v.split(",")[0].replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, "").toLowerCase() === "nosniff";
}
/* MIME Sniffing §6 "Matching a MIME type pattern", and the two tables of §7.1 "Identifying a resource with an
   unknown MIME type" whose answers a reader of `computedType` can act on.
   This mirrors `engine/host/browser/core/mime/mime_sniff.c` and is not a rival to it: that component is in
   the WASM sandbox and answers HTML §7.4.5's computed type for a navigation the engine parses itself, this is
   the trusted zone, and no call crosses between them. The two carry the same citations and the same cell
   encoding, so a reader repairing one finds the other by section number.
   One array carries both of §6's columns (mask byte in bits 8-15, pattern byte in bits 0-7), so §6's first
   step, "Assert: pattern's length is equal to mask's length", holds by construction.
   §5.2 "Reading the resource header" bounds what §7 looks at to 1445 bytes; it does not truncate the body. */
var _MS_RESOURCE_HEADER_MAX = 1445;

/* §6.1 "Matching an image type pattern" — the table in the standard's own row order, which is also the order
   `mime_sniff.c`'s IMAGE_TABLE states it in. */
var _MS_IMAGE = [
  { cells: [0xFF00, 0xFF00, 0xFF01, 0xFF00], type: "image/x-icon" },   // a Windows Icon signature
  { cells: [0xFF00, 0xFF00, 0xFF02, 0xFF00], type: "image/x-icon" },   // a Windows Cursor signature
  { cells: [0xFF42, 0xFF4D], type: "image/bmp" },                      // the string BM
  { cells: [0xFF47, 0xFF49, 0xFF46, 0xFF38, 0xFF37, 0xFF61], type: "image/gif" },   // GIF87a
  { cells: [0xFF47, 0xFF49, 0xFF46, 0xFF38, 0xFF39, 0xFF61], type: "image/gif" },   // GIF89a
  { cells: [0xFF52, 0xFF49, 0xFF46, 0xFF46, 0x0000, 0x0000, 0x0000, 0x0000,
            0xFF57, 0xFF45, 0xFF42, 0xFF50, 0xFF56, 0xFF50], type: "image/webp" },  // RIFF, four bytes, WEBPVP
  { cells: [0xFF89, 0xFF50, 0xFF4E, 0xFF47, 0xFF0D, 0xFF0A, 0xFF1A, 0xFF0A], type: "image/png" },
  { cells: [0xFFFF, 0xFFD8, 0xFFFF], type: "image/jpeg" }              // SOI, then another marker's indicator
];
/* §6.4 "Matching an archive type pattern". */
var _MS_ARCHIVE = [
  { cells: [0xFF1F, 0xFF8B, 0xFF08], type: "application/x-gzip" },
  { cells: [0xFF50, 0xFF4B, 0xFF03, 0xFF04], type: "application/zip" },              // PK, then ETX EOT
  { cells: [0xFF52, 0xFF61, 0xFF72, 0xFF21, 0xFF1A, 0xFF07, 0xFF00], type: "application/x-rar-compressed" }
];

/* §6's pattern matching algorithm over one row's packed cells. Steps 3 and 4 advance past the `ignored` set,
   and every row of §6.1 and §6.4 states that column as None, so the two indices are one; a table with an
   ignored column may not be walked by this. */
function _msPatternMatch(input, cells) {
  var p, masked;
  if (input.length < cells.length) return false;                        // step 2
  for (p = 0; p < cells.length; p++) {                                  // steps 5-6
    masked = input[p] & ((cells[p] >> 8) & 0xFF);
    if (masked !== (cells[p] & 0xFF)) return false;
  }
  return true;                                                          // step 7
}
/* "Execute the following steps for each row row in the following table ... If patternMatched is true, return
   the value in the fourth column of row" — the shape §6.1 and §6.4 share. `null` is the standard's undefined. */
function _msTable(rows, header) {
  for (var i = 0; i < rows.length; i++)
    if (_msPatternMatch(header, rows[i].cells)) return rows[i].type;
  return null;
}
/* §7.1's image step then its archive step, in the standard's order, and null where neither answers.
   Named residual. Not covered: every other row of §7.1 — §6.2 "Matching an audio or video type pattern" with
   its signature algorithms, the sniff-scriptable table, the PostScript-and-BOM table and the
   no-binary-data fall-through — and §7's steps 1, 4 and 5-6. The two tables may be walked alone because no
   earlier row of §7.1 can match what they match (the scriptable rows open 0x3C, the other table's 0x25, 0xFE,
   0xFF 0xFE or 0xEF, and the only image row opening 0xFF is 0xFF 0xD8 0xFF). Next diff: §6.2's table and its
   §6.2.1 MP4, §6.2.2 WebM and §6.2.3 MP3-without-ID3 algorithms, mirrored from `mime_sniff.c`'s
   `sniff_av_pattern`, together with §7.1's PostScript-and-BOM table, because §6.2.3's MP3 sync matches the
   UTF-16LE BOM 0xFF 0xFE and §7.1 orders the BOM table first. Its absence shows as an endpoint record whose
   reply carried no `Content-Type` (or an unknown essence) and whose bytes are a media stream. */
function _msUnknownTypePattern(header) {
  var m = _msTable(_MS_IMAGE, header);
  if (m) return m;
  return _msTable(_MS_ARCHIVE, header);
}

// CORB's own sniff, over bytes: a check decoding the evidence it judges, as Chrome's ORB does (it attempts a
// JSON parse). The head is decoded first and the whole body only where the head starts like JSON and does not
// parse, so a large JS chunk costs one 4 KiB decode and most API JSON is parsed once.
// It answers three questions from one pass. `protected` is CORB's: may these cross-origin bytes reach a code
// loader (markup and JSON may not). `type` is whether the bytes contradict a label this zone acts on, so its
// only non-null value is `application/json`, which the real parser confirmed; a leading `<` names no type,
// since no standard says which markup it is. `unknownType` is MIME Sniffing §7 "Determining the computed MIME
// type of a resource"'s step 2 arm, the one case where the bytes may name what the server did not; it is
// computed over §5.2 "Reading the resource header"'s raw bytes, because §6.1's and §6.4's rows ignore no
// leading bytes.
function _sniff(bytes) {
  var dec = new TextDecoder("utf-8");   // strips a leading UTF-8 BOM
  var hdr = bytes.subarray(0, _MS_RESOURCE_HEADER_MAX);
  var unknown = _msUnknownTypePattern(hdr);
  var h = dec.decode(bytes.subarray(0, 4096)).replace(/^﻿/, "").replace(/^\s+/, "");
  if (h.charAt(0) === "<") return { protected: true, type: null, unknownType: unknown };   // HTML/XML/SVG/markup
  if (h.charAt(0) === "{" || h.charAt(0) === "[") {
    try { JSON.parse(h); return { protected: true, type: "application/json", unknownType: unknown }; }
    catch (e) {
      try {
        JSON.parse(new TextDecoder("utf-8").decode(bytes));
        return { protected: true, type: "application/json", unknownType: unknown };
      }
      catch (_) {}
    }
  }
  return { protected: false, type: null, unknownType: unknown };
}
// What this response is, as one statement the renderer is told: MIME Sniffing §4.2 "MIME type
// miscellaneous"'s essence of the server's `Content-Type` when it stated one, and what the bytes say when it
// did not or when they contradict it with JSON. The empty string is MIME Sniffing §5.1 "Interpreting the
// resource metadata"'s "the supplied MIME type is undefined" surviving the sniff: a positive answer that
// neither the server nor the bytes named a type. Nosniff is final: the server's label is returned unchanged.
//
// A declared type the bytes contradict is otherwise the server's to name. MIME Sniffing §7 reaches the bytes
// only at step 2 (undefined or unknown type), step 4 (the apache-bug flag) and steps 5-6 (refining an image or
// audio-or-video type), and its last step is "The computed MIME type is the supplied MIME type", so a PNG
// served `text/plain` computes `text/plain` in every browser. That is why `extension/lib/discovery.js`'s
// `classifyResponseAsset`, where magic bytes are authoritative for a different question (is this body worth
// extracting a schema from), must not be routed to from here. §7 never sniffs a font: §6.3 "Matching a font
// type pattern" is invoked only by §8.7 "Sniffing in a font context".
function _computedType(declared, nosniff, sniff) {
  var mime = String(declared == null ? "" : declared).split(";")[0].trim().toLowerCase();
  DCHECK(sniff !== null && typeof sniff === "object" &&
         (sniff.unknownType === null || typeof sniff.unknownType === "string"),
         "the sniff handed to `_computedType` carries no §7.1 answer — `_sniff` writes that field on every " +
         "arm, as `null` for a resource whose bytes match no pattern it walks, so an absent one is a SECOND " +
         "sniff from somewhere else and the arm below would silently take the answer that has no bytes " +
         "behind it");
  // MIME Sniffing §7 step 2, asked before step 3's nosniff because the standard asks it there: "If the supplied
  // MIME type is undefined or if the supplied MIME type's essence is "unknown/unknown", "application/unknown",
  // or "*/*", execute the rules for identifying an unknown MIME type with the sniff-scriptable flag equal to
  // the inverse of the no-sniff flag and abort these steps." So nosniff with no `Content-Type` narrows which of
  // §7.1's tables may run rather than stopping the algorithm; neither table walked here is in the
  // sniff-scriptable half, so the flag gates nothing yet, and the DCHECK below makes that loud the day one is.
  // This quotation is in line comments because its third essence would close a block comment.
  if (!mime || mime === "unknown/unknown" || mime === "application/unknown" || mime === "*/*") {
    if (sniff.unknownType) {
      /* It cannot fail over the two tables above and can over the next ones: §7.1's sniff-scriptable table
         returns `text/html`, `text/xml` and `application/pdf`, and §7.2's note says these rules must "never
         determine the computed MIME type to be a scriptable MIME type, as this could allow a privilege
         escalation attack". `solver/engine.c` reads `computedType` to decide whether reply bytes are queued
         as a program, so a row added without §7.1's flag would be that escalation; this fires instead. */
      DCHECK(!_jsMime(sniff.unknownType) && !_corbProtectedMime(sniff.unknownType),
             "§7.1's unknown-type sniff answered `" + sniff.unknownType + "`, which is a JavaScript or a " +
             "CORB-protected type — the tables walked here are §6.1's and §6.4's and neither can produce one, " +
             "so a row was added from §7.1's SNIFF-SCRIPTABLE table without the flag that gates it, and a " +
             "reply is one `computedType` read away from being compiled as a program");
      return sniff.unknownType;
    }
  }
  if (nosniff) return mime;
  if (!mime) return sniff.type || "";
  // The bytes contradict the label: a JSON body under `text/plain` or a JavaScript type is JSON, and saying so
  // lets the reply be learned from rather than skipped as whatever the server mislabelled it.
  if (sniff.type && sniff.type !== mime) return sniff.type;
  return mime;
}
// Is this request's destination script-like, in Fetch §2.2.5 "Requests"' own words: "A request's destination
// is script-like if it is `audioworklet`, `paintworklet`, `script`, `serviceworker`, `sharedworker`, or
// `worker`." That predicate is the CORB question, asked of the destination the engine put on the request.
// `xslt` is deliberately outside it: the spec's note says such algorithms "should also consider `xslt` as that
// too can cause script execution", and considering it here yields exclusion, because this rule demands a
// JavaScript MIME type and an XSLT stylesheet is XML. Loading XSLT needs its own rule.
function _isScriptLike(d) {
  return d === "audioworklet" || d === "paintworklet" || d === "script" ||
    d === "serviceworker" || d === "sharedworker" || d === "worker";
}
// Is this request a subresource of the document: a second question asked of the same destination string, for
// the egress policy (whose act the request is) rather than for CORB (may the reply run as code). The two part
// at a stylesheet, which is CORB data and yet the page loading itself.
// It is Fetch §2.2.5's subresource request — "a request whose destination is audio, audioworklet, font,
// image, json, manifest, paintworklet, script, style, text, track, video, xslt, or the empty string" — minus
// the empty string, and the minus is the decision: the empty string is what a `fetch()` and an XHR carry, so
// the term as written would admit every data request at every origin. It is the positive list, never the
// complement of another set, because §2.2.5's sets do not partition its enumeration (`webidentity` is in
// neither).
// Named residual. Not covered: a subresource the page's markup or code names whose destination is the empty
// string, which this chokepoint cannot separate from a data fetch. Next diff: the park kind on the pending
// line beside the destination (`engine_pending_fetches` joins method, destination, initiator, provenance,
// pinned, credentials and URL), saying whose algorithm is owed the reply. Its absence shows as a
// `blocked-signal:destination=value` refusal for an address the analysed document's own markup declares.
function _isDocumentSubresource(d) {
  return d === "audio" || d === "audioworklet" || d === "font" || d === "image" ||
    d === "json" || d === "manifest" || d === "paintworklet" || d === "script" ||
    d === "style" || d === "text" || d === "track" || d === "video" || d === "xslt";
}
// Fetch §2.2.5 "Requests"' destination type, enumerated: "A destination type is one of: the empty string,
// `audio`, `audioworklet`, `document`, `embed`, `font`, `frame`, `iframe`, `image`, `json`, `manifest`,
// `object`, `paintworklet`, `report`, `script`, `serviceworker`, `sharedworker`, `style`, `text`, `track`,
// `video`, `webidentity`, `worker`, or `xslt`." It lives beside the predicates over it because this file is
// the one that decides from the value.
var _DESTINATION_TYPES = ["", "audio", "audioworklet", "document", "embed", "font", "frame", "iframe",
                          "image", "json", "manifest", "object", "paintworklet", "report", "script",
                          "serviceworker", "sharedworker", "style", "text", "track", "video",
                          "webidentity", "worker", "xslt"];
/* Every caller must state a destination §2.2.5 defines (`_destinationOf`, a CHECK). "" is legitimate and
   means data; what must not happen is an absent or misspelled value reading as an answer, because
   `_isScriptLike` is false for anything it does not recognise, so both take the arm that skips CORB for a
   code load. It is a CHECK and not a DCHECK because with it compiled out that fail-open arm is what release
   runs, on a security boundary. The value also crosses from the untrusted engine, whose own DCHECKs on the
   enumeration are compiled out on the far side of a mojo boundary; asserting it at the chokepoint covers the
   extension and `engine/trusted.mjs` at once. */
/* Does a gate in this file read this request's whole body. Derived from `_isScriptLike`, never restated,
   because CORB is the only gate that reads the body and its scope is that predicate. A first-chunk sniff is
   not a weaker answer but the opposite one: `_sniff` falls back to parsing the whole body, so a JSON document
   longer than 4096 bytes fails the prefix parse and a first-chunk CORB would admit it to a code loader. So a
   script-like destination is refused streaming before the request is made, graded `decline` because a
   browser streams a script and only this tool declines. */
function _bodyGated(opts) {
  return _isScriptLike(_destinationOf(opts));
}
function _destinationOf(opts) {
  CHECK(typeof opts.destination === "string" && _DESTINATION_TYPES.indexOf(opts.destination) >= 0,
        "safeFetch was called with a DESTINATION that is not one Fetch §2.2.5 \"Requests\" enumerates: " +
        JSON.stringify(opts.destination) + " — §2.2.5 gives every request one (\"unless stated otherwise it " +
        "is the empty string\") and this file decides the CORB class from it by asking §2.2.5's own " +
        "script-like predicate. A value that predicate does not recognise — absent, misspelled, or invented " +
        "by a compromised renderer — takes the `not script-like` arm, which is how a code load gets fetched " +
        "as data and a cross-origin HTML or JSON body reaches a compiler; state \"\" and mean it, or state " +
        "what the request is for");
  return opts.destination;
}
/* Fetch §2.2.5 "Requests"' mode — "A request has an associated mode, which is `same-origin`, `cors`, `no-cors`,
   `navigate`, `websocket`, or `webtransport`." Stated by the algorithm that created the request (the engine
   states it on the pending line and the XHR record; a §7.4 navigation is `navigate`) and never inferred from
   the address. Fetch §4.1 "Main fetch" decides from it what the page may read of the reply: a `cors` reply
   passes §4.10 "CORS check", a cross-origin `no-cors` reply becomes an opaque filtered response, and a
   `same-origin` request to another origin is a network error. A CHECK, because the arm an absent or invented
   mode would take is the one that hands the page another origin's bytes. A `websocket` request is the
   opening handshake (WebSockets §2.2 "Opening handshake"), judged here as every request is and never sent:
   this chokepoint opens no connection, so `_safeFetchJudged` declines an admitted one where the wire would be
   asked. `webtransport` is not a request this chokepoint performs. */
var _MODE_TYPES = ["same-origin", "cors", "no-cors", "navigate", "websocket", "webtransport"];
function _modeOf(opts) {
  CHECK(typeof opts.mode === "string" && _MODE_TYPES.indexOf(opts.mode) >= 0,
        "safeFetch was called with a MODE that is not one Fetch §2.2.5 \"Requests\" enumerates: " +
        JSON.stringify(opts.mode) + " — the mode decides whether the reply is CORS-checked, opaque, or " +
        "refused for another origin, so a request that does not state the one its creating algorithm named " +
        "would be answered by whichever arm reads it as the default");
  CHECK(opts.mode !== "webtransport",
        "safeFetch was called with the mode `webtransport`, which is a session this chokepoint does not open " +
        "— a WebTransport session is the page's own (intercept.js), never a GET here");
  return opts.mode;
}
// What a reply is evidence of: `observed`, `derived` or `forced`. The engine states the word (solver/engine.h's
// PENDING_PROVENANCE_*, composed at the park from the parking flow's `path_forced` and from HTML §4.12.1.1
// "Processing model"'s parser-inserted script, a `script` whose parser document is non-null); a zone that
// originated an act states it for itself. This file decides from it and may not re-derive it: nothing in an
// address distinguishes a request a session would have made from one that exists because a gate was forced.
var _PROVENANCE_TYPES = ["observed", "derived", "forced"];
// Every caller must state one, and it is a CHECK: the firing decision and the deny list's scope rest on it,
// and a missing or invented word must not be answered by whichever arm it happens to fall to. The value
// crosses from the untrusted engine, and asserting it at the chokepoint covers both hosts. There is no
// `unknown` grade: a request whose provenance is not established crashes at the decision rather than loading.
function _provenanceOf(opts) {
  CHECK(typeof opts.provenance === "string" && _PROVENANCE_TYPES.indexOf(opts.provenance) >= 0,
        "safeFetch was called with a PROVENANCE that is none of the three CLAUDE.md " +
        "§A-REQUEST-CARRIES-THE-PROVENANCE declares (solver/engine.h's PENDING_PROVENANCE_*): " +
        JSON.stringify(opts.provenance) + " — this file decides whether to FIRE the request from it, and " +
        "an absent or invented value takes the same arm `derived` does, which is the arm that spends the " +
        "network. State `observed`, `derived` or `forced` and mean it");
  return opts.provenance;
}
/* The zone's record of the document holding this request (lib/document-record.js): its reach grade, drive
   mark and witness mark, written once when the zone provisioned it and never by the engine. It is a floor under
   the engine's per-request words: the `doc-reach` row reads its grade, a `page` request from a driven document
   reads `actor=drive`, and one from a pinned document reads `witness=pinned`, whatever the engine's record
   states. A CHECK for the provenance reason: an absent record would make every default arm's conjunction
   unsatisfiable and let a driven or pinned document's request read as the page's own. */
function _docOf(opts) {
  CHECK(docRecordIs(opts.doc),
        "safeFetch was called without the zone's record of the DOCUMENT this request was made from: " +
        JSON.stringify(opts.doc) + " — mint it with docRecord where the document was provisioned. " +
        "It is the floor under the engine's own words, and a caller that states only the request's marks is " +
        "describing half the act");
  return opts.doc;
}
/* The witness mark the engine states beside the provenance: solver/engine.h's `pinned`/`unpinned`, from
   solver/flow.h's `path_pinned`, composed at the park for a request that parks and at the act
   (`engine_pinned_of_running_path`) for one the page's code made without parking. It says whether the flow
   had determined some source's value on an arm nothing observed before it built this address. It is a
   may-rest-on: concretize-on-pin answers a pinned read with a bare primitive, so the address cannot be asked
   where its bytes came from, and the engine records the fact where they are chosen instead. `unpinned`
   positively states that every byte came from the document, the server or the bundle's own text. A CHECK
   for the provenance reason. */
/* `unstated` is this zone's third word, for an act whose record carries no mark; writing `unpinned` there
   would be false exactly where it matters. A caller states `unpinned` only for an address no engine path
   composed, or from its record's mark; any other act states `unstated`. The mark is independent of the
   provenance, so a `derived` act may rest on a witness. An absent mark still aborts, so `unstated` is always
   typed by a caller that means it, and `unstated` and `unpinned` are separate rows a person permits separately.
   The navigation records (core/frame/navigable.c's load job and create notice, solver/route_seed.c's
   declaration) carry the engine's mark like the pending line does. */
var _PINNED_MARKS = ["pinned", "unpinned", "unstated"];
function _pinnedOf(opts) {
  CHECK(_PINNED_MARKS.indexOf(opts.pinned) >= 0,
        "safeFetch was called with a PINNED mark that is none of the three this file declares: " +
        JSON.stringify(opts.pinned) + " — this file decides whether to SPEND AN ACT on an address from it, " +
        "and an absent or invented value would take the permissive arm exactly as a missing provenance " +
        "would. State `pinned` or `unpinned` from the engine's pending line, or `unstated` if this act does " +
        "not carry one");
  return opts.pinned;
}
/* Whose act this request is: the analysed page's or this tool's. Every other row describes the request and
   this one describes the caller: the page's own `fetch()` and this tool's automatic discovery sweep are
   identical on destination, provenance, doc-reach, witness and cookies, and differ only in who composed
   them. `header-authority` differs between them only because the sweep passes `headers || {}`, and the
   page's XHR states a list, so it cannot stand in for this row.
   It is a value typed at each asker and CHECKed here, never inferred from who the callers are, and an absent
   one aborts. The words are deliberately not `lib/schema.js`'s `PAGE_CONTEXT_*`, which answer human or tool
   for an operator send (`safeFetchOperator`); a page's `fetch()` is neither. It is the one gating fact the untrusted zone cannot
   reach, a literal in trusted-zone source, hence `certain`. A request this tool composed at a person's
   direction (`trusted.mjs`'s seed, `bridge.js`'s residue re-fetch) is still `tool`; its human authorization
   belongs on another axis. */
var _ACTOR_WORDS = ["page", "tool"];
function _actorOf(opts) {
  CHECK(_ACTOR_WORDS.indexOf(opts.actor) >= 0,
        "safeFetch was called with an ACTOR that is neither word this file declares: " +
        JSON.stringify(opts.actor) + " — it says whether the ANALYSED PAGE'S own code composed this request " +
        "or THIS TOOL did, which is the fact that separates a page's own `fetch()` from a probe this tool " +
        "built, and no other signal here can: those two arrive identical on destination, provenance, " +
        "doc-reach, witness and cookies. Every caller knows which it is by construction — a relay answering " +
        "the engine's own record states `page`, and a composer that built the address itself states `tool` " +
        "— so there is no third word and an absent one is a site that never answered the question rather " +
        "than an act that cannot");
  return opts.actor;
}
/* The engine's drive mark (solver/engine.h's PENDING_DRIVEN_*): `driven` when the request was minted inside the
   engine's own drive of a function the page never called, `undriven` otherwise. It is a separate fact from the
   provenance (a drive that forced no arm is `derived`), and the `actor` row reads it alone: a `page` request
   that is `driven` is this tool's act. `unstated` is this zone's word for an act no engine record describes,
   and only a `tool` asker may state it, so every relay of an engine record carries the mark it was given. */
var _DRIVEN_MARKS = ["driven", "undriven", "unstated"];
function _drivenOf(opts) {
  CHECK(_DRIVEN_MARKS.indexOf(opts.driven) >= 0,
        "safeFetch was called with a DRIVE MARK that is none of the three this file declares: " +
        JSON.stringify(opts.driven) + " — the `actor` row reads it, so an absent one could fire a drive's " +
        "request as the page's own. Relay `driven`/`undriven` from the engine's record, or state `unstated` " +
        "for an act this tool composed");
  CHECK(opts.actor !== "page" || opts.driven !== "unstated",
        "safeFetch was called with actor `page` and an unstated drive mark — a `page` asker relays an engine " +
        "record, and every such record carries the mark, so this caller dropped the field that decides whose " +
        "act the request is");
  return opts.driven;
}
/* The request's method (Fetch §2.2.1 "Methods"), normalized by the algorithm that created the request, and
   its body: null, text, or bytes. Every caller states both. Fetch §5.4 "Request class" refuses a GET or HEAD
   request a body, so one here is a caller's error. The verbs the `method` row names; any other token reads
   `other`. */
var _METHOD_VERBS = ["GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"];
function _methodOf(opts) {
  CHECK(typeof opts.method === "string" && _HTTP_TOKEN.test(opts.method),
        "safeFetch was called with a method that is no Fetch §2.2.1 token: " + JSON.stringify(opts.method) +
        " — every caller states the verb its request carries, and an absent one would be sent as a GET");
  return opts.method;
}
function _bodyOf(opts) {
  CHECK(opts.body === null || typeof opts.body === "string" || opts.body instanceof Uint8Array,
        "safeFetch was called with a body that is neither null, text nor bytes — every caller states the " +
        "request's body, and null is how one with none says so");
  CHECK(opts.body === null || (opts.method !== "GET" && opts.method !== "HEAD"),
        "safeFetch was handed a body on a " + opts.method + " request, which Fetch §5.4 refuses to create");
  return opts.body;
}
/* What the body's bytes are, the engine's word beside them (core/fetch/fetch.h's fetch_body_class_token):
   `concrete` when the page computed them, `example` when they hold an unknown's example, `unknown` when they
   are the display shape of an unknown the run has no bytes for, which is refused at every setting. A null
   body is `none` by the caller's own `body: null`; a caller with a body states the class, `concrete` only for
   bytes no engine path composed. A CHECK for the provenance reason. */
var _BODY_CLASSES = ["none", "concrete", "example", "unknown"];
function _bodyClassOf(opts) {
  if (opts.body === null) {
    CHECK(opts.bodyClass === undefined || opts.bodyClass === "none",
          "safeFetch was called with no body and the body class " + JSON.stringify(opts.bodyClass));
    return "none";
  }
  CHECK(opts.bodyClass === "concrete" || opts.bodyClass === "example" || opts.bodyClass === "unknown",
        "safeFetch was called with a body whose class is none this file declares: " +
        JSON.stringify(opts.bodyClass) + " — relay the engine's word for the bytes, or state `concrete` for " +
        "a body this zone composed");
  return opts.bodyClass;
}
/* The address class the engine states beside the witness mark (core/fetch/fetch.h's fetch_address_token, on
   the pending line and the XHR record): `concrete` when the page's code computed the bytes, `example` when the
   address holds an unknown and the bytes are what the page computes from that unknown's example. `unknown` is an
   address with no bytes, which the engine declines itself and never lists; a record stating it is refused at
   every setting. `unstated` is this zone's word for an engine record that carries no class. A caller states
   `concrete` only for an address no engine path composed. A CHECK for the provenance reason.
   Named residual. Not covered: a navigation's record (core/frame/navigable.c's load job and notices,
   solver/route_seed.c's declaration) carries no class, so its request states `unstated`. Next diff: those
   records carry fetch_address_token. Its absence shows as every engine-named navigation's entry reading
   `address=unstated`. */
var _ADDRESS_CLASSES = ["concrete", "example", "unknown", "unstated"];
function _addressOf(opts) {
  CHECK(_ADDRESS_CLASSES.indexOf(opts.address) >= 0,
        "safeFetch was called with an address class that is none this file declares: " +
        JSON.stringify(opts.address) + " — relay `concrete` or `example` from the engine's record, state " +
        "`unstated` for a record that carries none, or `concrete` for an address this zone composed");
  return opts.address;
}
/* A header value's class, the third member of each entry of a stated header list (`[name, value, class]`): the
   engine's word for that value's bytes (core/fetch/headers.h's header_value_class_token), `concrete` for a list
   this zone composed. The class rides the value it is about, so a redirect that drops a header drops its class. */
var _HEADER_VALUE_CLASSES = ["concrete", "example", "unknown"];
function _headerListIs(list) {
  return Array.isArray(list) && list.every(function (h) {
    return Array.isArray(h) && h.length === 3 && typeof h[0] === "string" && typeof h[1] === "string" &&
           _HEADER_VALUE_CLASSES.indexOf(h[2]) >= 0;
  });
}
/* The same test for a host, which reads an engine's header list as input and refuses one this file would
   abort on. */
function safeFetchHeaderListIs(list) {
  return _headerListIs(list);
}
/* What the request's bytes are: the weakest class over every part the engine composed (the address, each header
   value, the body). `unknown` is weakest (a part with no bytes), then `unstated` (a part whose record carries
   no class), then `example`, then `concrete`; a null body and an absent header list are no part. */
var _BYTES_CLASSES = ["concrete", "example", "unknown", "unstated"];
function _bytesClassOf(f) {
  var parts = [f.address].concat((f.headers || []).map(function (h) { return h[2]; }),
                                 f.bodyClass === "none" ? [] : [f.bodyClass]);
  return ["unknown", "unstated", "example"].find(function (w) { return parts.indexOf(w) >= 0; }) || "concrete";
}
/* The class of each part, for the fired-send entry: the row above says whether the request has bytes, and this
   says which part held the class it states. A header is named, never quoted. */
function _bytesParts(f) {
  return { address: f.address, headers: (f.headers || []).map(function (h) { return [h[0], h[2]]; }),
           body: f.bodyClass };
}
/* What this function reads. A field outside this set is refused rather than dropped in silence and read by
   its caller as carried; `as` is refused by it, since the CORB class is the request's destination. A closed
   set restates what the body reads, and an option added to the body and not here aborts on its author's
   first call. */
var _SAFEFETCH_OPTIONS = ["method", "body", "bodyClass", "pageOrigin", "destination", "mode", "provenance", "pinned", "driven", "address",
                          "doc", "actor", "credentialed", "credentials", "headers", "useCorsPreflight",
                          "identity", "signal", "onChunk", "onBodyRead", "onHead"];
/* Fetch §2.2.5 "Requests"' credentials mode — "which is `omit`, `same-origin`, or `include`" — the same three
   words `fetch_credentials_token` writes on the pending line. Written here because this zone is the other
   party to that contract. */
var _CREDENTIALS_MODES = ["omit", "same-origin", "include"];
/* A DCHECK: release can proceed, because every unread option lands on the safe side (a dropped `method` or
   `body` fires the GET this file always fires); what is lost is the caller's belief about what it sent. The
   facts the firing walk decides from are CHECKed at their own reads. It may assert at all because the keys
   are trusted-zone literals at every call site and the untrusted engine supplies only values, so no bundle or
   renderer can reach it. */
function _refuseUnreadOptions(opts) {
  for (var k in opts) {
    if (!Object.prototype.hasOwnProperty.call(opts, k)) continue;
    DCHECK(_SAFEFETCH_OPTIONS.indexOf(k) >= 0,
           "safeFetch was passed the option `" + k + "`, which it does not read — it would be dropped in " +
           "silence and the request would not carry what its caller said about it. State only what this " +
           "file reads (" + _SAFEFETCH_OPTIONS.join(", ") + ")");
  }
}
/* Whether the person's session pays for this request, and the one option that may not ride along with it.
   `credentialed` is this zone's own literal; nothing on the wire states it. It scopes the deny list and the
   credentialed SOP. `opts.headers` is the one option whose values come from the untrusted zone: the bundle's
   own list on the XHR path, which on an uncredentialed GET confers nothing the page lacks.
   Together they are a different question. A credentialed request is a GET (`_safeFetchJudged` CHECKs it),
   and a header list is another route to a verb: with `X-Http-Method-Override` (a convention
   `lib/discovery.js` uses) a server reads this GET as the header's verb. So a credentialed request may not
   state a header list, a CHECK because with it compiled out that request goes out with cookies and untrusted
   header values. It may assert because the decision to ask for cookies is a trusted-zone literal at every
   call site. If it refuses someone, what is owed is a statement of whose header list it is, carried from the
   site that knows, not an exemption.
   `credentials` is a separate fact: Fetch §2.2.5's credentials mode, named by the algorithm that created the
   request and carried on the pending line. It can only narrow what this zone was already willing to do. */
function _credentialedOf(opts) {
  /* The flag is stated, not coerced. A DCHECK because the arm a compiled-out assert leaves is the
     uncredentialed one, so nobody's session is spent. It is asserted at all because an absent key and a
     stated `false` are one fact here and two at the call site, and stating it makes the header CHECK below
     test what a caller says. The key is a trusted-zone literal at every call site, so no bundle or renderer
     can reach the abort. The two CHECKs below are fatal because the arm they leave sends the cookies. */
  DCHECK(typeof opts.credentialed === "boolean",
         "safeFetch was called without stating whether the PERSON'S SESSION pays for this request. The " +
         "coercion below reads an absent flag as `false`, which is the safe arm and is why this is not " +
         "fatal — but absent and `false` are two different facts AT THE CALL SITE, and only one of them is " +
         "a decision somebody took. State `credentialed: false` (this zone is unwilling, or supplies no " +
         "`pageOrigin` for the credentialed SOP to match), or `credentialed: true` beside a " +
         "Fetch §2.2.5 \"Requests\" credentials mode. A caller that states neither hands the egress " +
         "surface a `cookies=no` row with no fact under it, which is the reading a person widens an " +
         "origin on");
  var credentialed = !!opts.credentialed;
  var mode = opts.credentials;
  CHECK(!(credentialed && opts.headers),
        "safeFetch was asked for a CREDENTIALED request that also states a header list — these are two " +
        "populations arriving through one parameter and only one of them has been decided. The header list " +
        "on the XHR path is the analysed BUNDLE's own, which is within the model while the request is " +
        "uncredentialed (a header on a request the page can already make itself confers nothing) and is a " +
        "different question the moment the person's cookies pay for it: a credentialed request is a GET " +
        "here, and a header list is the other route to a verb — " +
        "`X-Http-Method-Override` is a convention this project's own code sends, and a server honouring it " +
        "reads this GET as whatever the header names, which puts a state-mutating verb back on a " +
        "cookie-bearing request past the place this file closed it. State " +
        "whose header list it is at the site that knows, the way `provenance` is stated, or send it " +
        "uncredentialed");
  /* The second input is the request's own: `credentialed` is this zone's willingness to spend the session,
     `credentials` is the mode the creating algorithm stated, and the answer is their conjunction, since the
     zone cannot know an element's mode and the engine cannot know whether this person wants their session
     spent. A stated mode can only narrow: an engine that could turn cookies on would hold network policy. An
     unknown word is producer drift and fatal at any setting; an absent one is honest for a request this zone
     composed itself and may not stand on a cookie-bearing request.
     Named residual. Not covered: `same-origin` when this zone is willing; the only same-origin comparison is
     after the wire, over the landed origin (`_resourceSameOrigin`). Next diff: the same-origin test on the
     request's own URL against `pageOrigin`, read here, with the post-fetch gate keeping the redirect case.
     Its absence shows as the CHECK_FAIL below firing; every willing caller today is a navigation stating
     `include`. */
  CHECK(mode === undefined || _CREDENTIALS_MODES.indexOf(mode) >= 0,
        "safeFetch was called with a CREDENTIALS MODE that is none of Fetch §2.2.5 \"Requests\"' three: " +
        JSON.stringify(mode) + " — the engine spells them in exactly one place " +
        "(core/fetch/fetch.h's `fetch_credentials_token`, which is what writes this field onto the pending " +
        "line), so a fourth word is that spelling and this one having parted, and it would be answered here " +
        "by whichever arm happens to be last");
  CHECK(!credentialed || mode !== undefined,
        "safeFetch was asked for a CREDENTIALED request that states no Fetch §2.2.5 credentials mode — this " +
        "zone owns whether the person's session is spent and the algorithm that CREATED the request owns " +
        "what the request IS, and neither answers for the other. Spending a session on a request whose " +
        "creating algorithm never said whether it should is the credential question decided by silence, " +
        "which is the whole reason the mode crosses the seam. State it (`include` is HTML §7.4.5 " +
        "\"Populating a session history entry\"'s answer for a navigation), or send it uncredentialed");
  if (!credentialed) return false;
  /* §2.2.5's own arms in its own order: `omit` is the narrowing this parameter exists to make, and `include`
     is what every willing caller states. */
  if (mode === "omit") return false;
  if (mode === "include") return true;
  CHECK_FAIL("safeFetch was asked for a CREDENTIALED request whose Fetch §2.2.5 credentials mode is " +
             "`same-origin`, and that is a CONDITIONAL answer this file does not yet resolve BEFORE the " +
             "wire — see the paragraph above. It is fatal rather than narrowed to `omit` because a silent " +
             "narrowing here hands a same-origin document load the LOGGED-OUT body with nothing anywhere " +
             "saying so, which is the moat this tool exists to cross. Build the pre-request same-origin " +
             "test against `pageOrigin`; every willing caller today is a §7.4 navigation stating `include`");
}
/* The signals the egress policy surfaces, and the person who decides from them. Method, credential state,
   provenance, URL-carried authority, credential lineage, destination and intended invalidity are facts the
   policy shows, and the person decides per origin which values it allows. The control is per-signal rather
   than a score: a wrong weighting inside a score is invisible, while a named signal a person allowed is a
   decision they can revisit.
   No combination is refused at every setting, so the only thing between a person and a combination they did
   not intend is whether the control stated the consequence. A signal is shown only where this file can
   compute it, and one it cannot determine is shown as a stated unknown, never as an absence a default fills.
   They are separate rows because stripping cookies does not make a request uncorrelated with the person
   (capability URLs, lineage), GET is a signal about intent rather than a guarantee (RFC 9110 §9.2.1 "Safe
   Methods"), and a request built to fail validation is not thereby unsafe.
   The registry lives here because the decision does: `engine/trusted.mjs` loads this file verbatim and its
   `--explore <origin>` writes here, and the offscreen restores a person's grants into the same table. A
   permission buys the request; carrying the reply to a forced request as forced, never merged into the
   observed pool, is the engine's obligation. */
/* How reliable each signal is, declared beside it, so a person ticking a row knows whether they permit
   something this zone knows, something another zone said, or something nobody established.
     "certain"      — this zone computes the value from its own literals and code.
     "stated"       — another zone determined it; the CHECKs assert only the vocabulary, so a wrong word
                      inside it is a claim this zone relays, not a fact it holds.
     "partial"      — a lower bound: one value is a fact when it appears, and the other is a stated unknown,
                      never the first one's negative.
     "undetermined" — nothing in this zone can compute it, so its only value is `unknown`; it is shown anyway,
                      because permitting a request whose lineage nobody established differs from permitting
                      one known to have none.
     "intent"       — an intent the engine holds and cannot verify: a probe built to fail validation cannot be
                      known to be refused before business logic. */
var _SIGNAL_CERTAINTY = ["certain", "stated", "partial", "undetermined", "intent"];
/* URL-carried authority: a match in the deny direction only. Matching is banned because a matched name
   would be asserted as a value; a match that only refuses asserts nothing. A hit is a fact and a miss is not
   a finding: a miss reads `unknown`, never `none`.
   The list is what authorizes a stranger without a session: presigned object-store signatures, reset and
   invite tokens, signed webhooks. `key` and `api_key` are excluded: an API key is the application's
   authority, not the person's, and admitting it would make `present` the common answer and train readers to
   ignore the row. A bare opaque token in a path segment is indistinguishable from an object id; the JWS
   prefix below is the one self-identifying path-borne form.
   Named residual. Not covered: authority in a path segment that is not a JWS (an opaque `/invite/<40 hex>`
   reads `unknown`). Next diff: read the signal off the engine, whose park knows which source each segment
   came from, so a segment from a credentialed reply answers this and lineage together. Its absence shows as
   `url-authority=unknown` doing all the work at a widened origin whose addresses carry capability tokens,
   while the `present` row is never exercised. */
var _URL_AUTHORITY_PARAMS = ["x-amz-signature", "x-goog-signature", "x-ms-signature", "sig", "signature",
                             "hmac", "token", "access_token", "auth_token", "reset_token", "invite_token"];
function _urlAuthorityMarker(u) {
  var found = null;
  u.searchParams.forEach(function (v, n) {
    if (found === null && _URL_AUTHORITY_PARAMS.indexOf(String(n).toLowerCase()) >= 0) found = String(n).toLowerCase();
  });
  if (found !== null) return found;
  /* A JWS compact serialization anywhere in the path or query. RFC 7515 §3.1 "JWS Compact Serialization
     Overview" makes the first component `BASE64URL(UTF8(JWS Protected Header))` and §4 "JOSE Header" makes
     that a JSON object, so it begins `{"` and a member name; base64url of those three bytes is `eyJ` whenever
     the name begins with an ASCII letter or `_` (`{"a` is `eyJh`, while `{"0` is `eyIw`). Every registered
     JOSE header name begins with a letter. A lower bound like every other entry: a miss is `unknown`. */
  if ((u.pathname + u.search).indexOf("eyJ") >= 0) return "jws";
  return null;
}
/* The registry: one declaration, read by the firing walk and by the surface. The order is the surface's, and
   the refusal's only among rows the deciding rule does not name (`_firingDecision`). `gates` is asserted
   rather than styled: a non-gating signal has exactly one value at this chokepoint, and
   `_signalRegistryCheck` aborts host startup if one grows a second. A non-gating signal is still shown,
   because it is a fact a person is entitled to see before permitting anything. */
var _SIGNALS = [
  /* Fetch §2.2.5 "Requests"' destination, as three classes, each by its own predicate over the one string:
     `_isScriptLike` answers CORB's "is this code", a document's other subresources are `subresource`, and
     every other request is `value`, a navigation included. No default arm reads it: whose act a request is
     does not depend on what its reply becomes. It is a row a person permits at a widened origin.
     The cascade order is load-bearing because §2.2.5's sets overlap: `script`, `audioworklet` and
     `paintworklet` are script-like and subresources, while `serviceworker`, `sharedworker` and `worker` are
     script-like only. Asking the CORB set first keeps all six reading `program`. */
  { name: "destination", gates: true, certainty: "stated",
    values: ["program", "subresource", "value"],
    of: function (f) {
      if (_isScriptLike(f.destination)) return "program";
      return _isDocumentSubresource(f.destination) ? "subresource" : "value";
    } },
  /* Whose act this request is: the caller's word (see `_actorOf`), with a `page` request the engine states it
     minted inside a flow this tool drives (`driven`: a drive of uncalled code or a candidate run, see
     `_drivenOf`), or made from a document this zone recorded as reached by one (`_docOf`), reading `drive`. The
     page's code composed such a request and this tool chose to run that code, so it is neither the page's own
     act nor a request this zone composed. `certain`: the caller's word is a literal in trusted-zone source,
     and the engine's mark can only move `page` to `drive`. */
  { name: "actor", gates: true, certainty: "certain", values: ["page", "tool", "drive"],
    of: function (f) {
      return f.actor === "page" && (f.driven === "driven" || f.doc.driven === "driven") ? "drive" : f.actor;
    } },
  /* The request's provenance (see `_provenanceOf`). `stated`: the engine composes it at the park from the
     park's kind and the flow's `path_forced`, and the CHECK catches only a word outside the three. */
  { name: "provenance", gates: true, certainty: "stated", values: _PROVENANCE_TYPES,
    of: function (f) { return f.provenance; } },
  /* How the document holding this request was reached (the grade of `_docOf`'s record): the same three words about a
     different act. A page this tool chose to open makes its own `fetch()`es, each `provenance=observed`, so
     this row is what stops one permission answering for both. It follows `provenance` so the one-level-out
     sentence comes next. `stated`: the zone that performed the load composes it, and the CHECK asserts only
     the vocabulary. */
  { name: "doc-reach", gates: true, certainty: "stated", values: _PROVENANCE_TYPES,
    of: function (f) { return f.doc.reach; } },
  /* Whether the person's cookie jar pays, and it means cookies and nothing wider: `credentials:"omit"` does
     not strip an `Authorization` header, authority in the address, or lineage, which is why those are their
     own rows. `certain`: `_credentialedOf` composes it from this zone's literal and an asserted mode, and it
     decides what `init.credentials` says. */
  { name: "cookies", gates: true, certainty: "certain", values: ["yes", "no"],
    of: function (f) { return f.credentialed ? "yes" : "no"; } },
  /* Whether other authority rides in a header, which is a question about whose list it is. On the XHR path
     `opts.headers` is the bundle's own list, which may carry a bearer token the person's session minted, and
     nothing states its owner. So `none` is a fact (with no list this zone adds no authority) and `unknown` is
     a stated unknown. Not `partial`, because here the informative value is the absence. */
  { name: "header-authority", gates: true, certainty: "certain", values: ["none", "unknown"],
    of: function (f) { return f.headers ? "unknown" : "none"; } },
  /* solver/engine.h's witness mark: whether the flow had determined some source's value on an arm nothing
     observed before it built this address, so the address may carry a witness the engine picked. Composed at
     the park from `flow_path_pinned`, or at the act (`engine_pinned_of_running_path`) for a request the
     page's code made without parking. `unstated` is this zone's word for an act whose record carries no mark
     (see `_pinnedOf`). A request from a document this zone recorded as reached on a pin reads `pinned`
     whatever the engine states (`_docOf`). */
  { name: "witness", gates: true, certainty: "stated", values: _PINNED_MARKS,
    of: function (f) { return f.doc.pinned === "pinned" ? "pinned" : f.pinned; } },
  /* Whether the request is bytes the page computed, or holds an unknown's example, over every part the engine
     composed (see `_bytesClassOf`). One row, because it is one question whichever part answers it. No default
     arm reads it: a session whose input is the example sends exactly this request. `never` lists the values no
     setting permits: a request with a part that has no bytes is refused before any arm is asked. */
  { name: "bytes", gates: true, certainty: "stated", values: _BYTES_CLASSES, never: ["unknown"],
    of: _bytesClassOf },
  /* Authority in the address (see `_urlAuthorityMarker`). `partial`: `present` is a fact, and `unknown` is not
     the statement that there is none. */
  { name: "url-authority", gates: true, certainty: "partial", values: ["present", "unknown"],
    of: function (f) { return _urlAuthorityMarker(f.url) !== null ? "present" : "unknown"; } },
  /* Credential lineage: do this address's bytes derive from a response fetched credentialed. The chokepoint
     cannot see it: it reads the credential state of the request in front of it, and an address the bundle
     computed from a credentialed reply (`/api/orgs/{orgId}/members/{userId}`) is about that person's account
     whatever headers it carries. It is shown as `unknown` rather than omitted, so a person's permission says
     they are permitting requests whose lineage nobody established. It is not what `doc-reach` answers: that
     row is about the act that reached the document, and neither bounds the other.
     Named residual. Not covered: every request; the only value is `unknown`. Next diff: a `credentialed` bit
     on the response the engine ingests, carried into the concolic value's source identity and composed at the
     park beside the provenance word. Its absence shows as every request at an origin reading
     `lineage=unknown`, including addresses the bundle demonstrably built from a logged-in reply. */
  { name: "lineage", gates: true, certainty: "undetermined", values: ["unknown"],
    of: function () { return "unknown"; } },
  /* Whose server the request reaches: the attacker site the person configured as their own
     (lib/attacker-site.js's attackerSiteReaches, on the URL parser's host), or another party's. Last of the
     gating rows, so a refusal names it only where every row before it is permitted. `certain`: the zone parses
     the address and compares it with its own record of the person's setting. */
  { name: "reaches", gates: true, certainty: "certain", values: ["attacker-site", "another-party"],
    of: function (f) { return attackerSiteReaches(f.url); } },
  /* The request's method (see `_methodOf`). No default arm reads it: the page's own unforced run sends the
     verbs a browser loading the page sends. `defaultOnly` lists the values only a default arm admits: no
     grant can name them, so this tool never sends a non-GET of its own, a forced one at a widened origin
     included (RFC 9110 §9.2.1 "Safe Methods": only a safe method is sent to learn). */
  { name: "method", gates: true, certainty: "stated", values: _METHOD_VERBS.concat(["other"]),
    defaultOnly: _METHOD_VERBS.slice(1).concat(["other"]),
    of: function (f) { return _METHOD_VERBS.indexOf(f.method) >= 0 ? f.method : "other"; } },
  /* Intended invalidity: an intent the engine holds and cannot verify. No caller can state one (the option
     set has no such field), so `unstated` is a structural fact, not a default; the population lives on
     `safeFetchOperator`. `_signalRegistryCheck` asserts the row has one value while it does not gate. */
  { name: "invalidity", gates: false, certainty: "intent", values: ["unstated"],
    of: function () { return "unstated"; } }
];
/* The registry is checked once, at the door every host goes through (`safeFetchEgressStated`), on what a
   grant is keyed by: a duplicate name would let one row's permission answer for another's; an unknown
   certainty renders as a blank where a person reads how far to trust a row; and a non-gating signal with
   more than one value is a dropped control, since every grant would silently permit both. */
function _signalRegistryCheck() {
  var seen = Object.create(null), i, s;
  _SITE_SETTINGS.forEach(function (t) {
    CHECK(safeFetchSignalUsable(t.name, t.values[0]) === "not-a-signal",
          "the site setting `" + t.name + "` shares its name with an egress signal, so one table row " +
          "would answer both");
  });
  for (i = 0; i < _SIGNALS.length; i++) {
    s = _SIGNALS[i];
    CHECK(!seen[s.name],
          "the egress signal `" + s.name + "` is declared twice — a grant is keyed on the NAME, so the " +
          "second declaration's permitted values would answer for the first's and a person's permission " +
          "would be about a row they never saw");
    seen[s.name] = true;
    CHECK(_SIGNAL_CERTAINTY.indexOf(s.certainty) >= 0,
          "the egress signal `" + s.name + "` declares the certainty " + JSON.stringify(s.certainty) +
          ", which is none this file states — the grade is what tells a person whether they are permitting " +
          "something this zone KNOWS, something another zone SAID, or something nobody established, and an " +
          "unrecognised one renders as a blank beside a checkbox they are about to tick");
    CHECK(Array.isArray(s.values) && s.values.length >= 1,
          "the egress signal `" + s.name + "` declares no value space — the surface renders one row per " +
          "value and a grant permits values, so a signal with none is a row a person can neither see nor " +
          "permit while the firing walk still asks it");
    CHECK(s.never === undefined || (Array.isArray(s.never) && s.gates && s.never.every(function (n) {
            return s.values.indexOf(n) >= 0; }) && s.never.length < s.values.length),
          "the egress signal `" + s.name + "` lists a never-permitted value outside its value space, or does " +
          "not gate, or permits no value at all");
    CHECK(s.defaultOnly === undefined || (Array.isArray(s.defaultOnly) && s.gates &&
            s.defaultOnly.every(function (n) { return s.values.indexOf(n) >= 0; }) &&
            s.defaultOnly.length < s.values.length),
          "the egress signal `" + s.name + "` lists a default-only value outside its value space, or does " +
          "not gate, or leaves no value a grant may name");
    CHECK(s.gates || s.values.length === 1,
          "the egress signal `" + s.name + "` does not gate and has more than one possible value — a signal " +
          "whose value VARIES and which no permission can express is a control that was silently dropped: " +
          "every grant permits every value of it and nothing anywhere says so. A signal is surfaced without " +
          "gating only where this chokepoint can produce exactly one answer, which is a structural fact " +
          "(`method` is a literal, `invalidity` cannot be stated through a closed option set) and stops " +
          "being true the moment that structure changes. Make it gate");
  }
  /* The default arms against the same registry. An arm naming an undeclared signal or value compares against
     `undefined` and is false for ever, silently revoking what it permitted (for the page's own `fetch()`,
     every waiting flow stays parked with no crash). A CHECK, because release cannot proceed correctly through
     an arm this build shipped broken. Exactly one arm is the rule a refusal is read against. */
  CHECK(_DEFAULT_ARMS.filter(function (a) { return a.decides === true; }).length === 1,
        "the default egress arms name no deciding rule, or more than one — a refusal names the fact of that " +
        "one rule a request failed, so with none it names a row of an empty table and with two it names " +
        "whichever was written first");
  for (i = 0; i < _DEFAULT_ARMS.length; i++) {
    CHECK(Array.isArray(_DEFAULT_ARMS[i].when) && _DEFAULT_ARMS[i].when.length >= 1,
          "a default egress arm states no condition — an arm is a permission matched on the conjunction it " +
          "names, and one naming none would fire for EVERY request at every origin, which is this file's " +
          "whole decision deleted by an empty array");
    _DEFAULT_ARMS[i].when.forEach(function (c) {
      var why = !Array.isArray(c.values) || c.values.length < 1 ? "no-values" : c.values.map(function (value) {
        return safeFetchSignalUsable(c.signal, value);
      }).find(function (w) { return w !== null; }) || null;
      CHECK(why === null,
            "a default egress arm names `" + c.signal + "=" + String(c.values) + "`, which this file's registry " +
            "answers `" + why + "` — an arm is matched by comparing the signal's computed value, so an " +
            "unknown name compares against `undefined` and an unknown value against nothing: the arm is " +
            "false for every request for ever, and what it was permitting is refused with no crash, no " +
            "diagnostic and a report that reads as a page with nothing behind it");
    });
  }
}
/* The facts every signal is computed from, required at the one derivation the firing walk and the surface
   share, so neither reader can be weaker than the other. Four unstated facts (`provenance`, `doc-reach`,
   `witness`, `actor`) compute outside their value space and the DCHECK in `_signalVector` would catch them;
   three compute a value inside it from nothing: an absent `destination` reads `value`, an absent
   `credentialed` reads `cookies=no`, an absent `headers` reads `header-authority=none`. The last two are
   `certain` rows, so a person would be shown an established fact nobody stated; release cannot proceed
   correctly through that, hence a CHECK.
   It may assert because every field is composed in trusted-zone source: the surface's caller builds a
   literal whose `url` `safeFetchWidenable` has answered for, and the engine-supplied values reaching
   `safeFetch` are CHECKed at their read. No page or renderer can reach this abort. */
function _requireFacts(facts) {
  CHECK(facts !== null && typeof facts === "object",
        "the signal vector was asked for with no facts — every signal is computed from them and a caller " +
        "that passed none would be answered by whichever arm each signal's predicate happens to reach first");
  CHECK(typeof facts.destination === "string" && _PROVENANCE_TYPES.indexOf(facts.provenance) >= 0 &&
        docRecordIs(facts.doc) &&
        _PINNED_MARKS.indexOf(facts.pinned) >= 0 && _ACTOR_WORDS.indexOf(facts.actor) >= 0 &&
        _DRIVEN_MARKS.indexOf(facts.driven) >= 0 && _ADDRESS_CLASSES.indexOf(facts.address) >= 0 &&
        _BODY_CLASSES.indexOf(facts.bodyClass) >= 0 &&
        typeof facts.method === "string" && facts.method !== "" &&
        typeof facts.credentialed === "boolean" &&
        facts.url !== null && typeof facts.url === "object" && typeof facts.url.origin === "string",
        "a signal vector was asked for without the facts that decide it — Fetch §2.2.5 \"Requests\"' " +
        "DESTINATION says " +
        "whether this reply becomes a PROGRAM or a VALUE, the actor says whether the ANALYSED PAGE composed " +
        "this request or THIS TOOL did, the drive mark says whether the engine's own drive minted it, the " +
        "provenance says what its path is evidence of, the reach " +
        "grade says whose act the DOCUMENT it was made from was, the witness " +
        "mark says whether the address may rest on a value this engine chose, the credential flag says " +
        "whether the person's session pays, and the parsed URL is what the address-borne authority is read " +
        "off. A caller that omitted any of them would be answered by the FIRING walk's permissive arm, " +
        "which is the one that spends an act, and shown on the SURFACE as a value with no fact under it: " +
        "an absent destination reads `value`, an absent credential flag reads `cookies=no` and an absent " +
        "header list reads `header-authority=none`, both of those on rows this registry grades `certain`. " +
        "Every one of them is a fact the CALLER holds and this file cannot " +
        "re-derive: forgetting to state one may never be a way to be exempted");
  /* The header list's shape: `header-authority` reads it by truthiness and grades `none` as `certain`, so a
     primitive (`""`, `0`, `false`) would be shown as an established absence. `null` and absent are how a
     caller with no list says so. Shape only, never contents: on the XHR path the names and values are the
     bundle's and are input. */
  CHECK(facts.headers === null || facts.headers === undefined || _headerListIs(facts.headers),
        "a signal vector was asked for with a header list that is not one: " +
        JSON.stringify(facts.headers) + " — the `header-authority` row reads this by TRUTHINESS and grades " +
        "the answer `certain`, so a primitive here is rendered to a person as the established fact that no " +
        "authority rides in a header, on a request whose header list was never stated. `null` is how a " +
        "caller with no list says so and is a fact; anything that is not an object is a call site that " +
        "meant something else. This asserts the SHAPE and never the CONTENTS — the names and values are " +
        "the analysed bundle's on the XHR path and are input this zone may not assert about. An entry is " +
        "`[name, value, class]`, the class one of " + _HEADER_VALUE_CLASSES.join(", "));
}
/* The vector for one request: every signal's value, computed from the facts required above. The firing walk
   and the surface both read it, so a person is never shown a value the walk did not decide from. */
function _signalVector(facts) {
  var v = Object.create(null), i, s, val;
  _requireFacts(facts);
  for (i = 0; i < _SIGNALS.length; i++) {
    s = _SIGNALS[i];
    val = s.of(facts);
    /* The computation and the declared value space must agree: a grant names declared values, so a value
       outside the space can never be permitted. A DCHECK, because release refuses it, the conservative arm. */
    DCHECK(s.values.indexOf(val) >= 0,
           "the egress signal `" + s.name + "` computed " + JSON.stringify(val) + ", which is not in the " +
           "value space it declares (" + s.values.join(", ") + ") — a grant names values out of that space, " +
           "so this request can never be permitted by any setting and the person's control has no row for " +
           "the answer it is being refused with");
    v[s.name] = val;
  }
  return v;
}
/* What leaves without anybody saying so, as data the surface shows. One rule decides for every kind of act (a
   value request, a navigation, a program or subresource load, a post to another document): the page's own act
   on its own unforced run. The destination is no part of it: a lazy chunk a forced branch asked for is a
   request the page did not make, and its reply is evidence about a request no client sends. The other two
   arms are no second rule: one reaches no other party, and one is the load a person asked this zone for.
   A refused request is still derived in full and recorded, and is sent once the person widens its origin.
   These are permissions only, so nothing here narrows the table. A condition holds when the signal's value is
   one of `values`; `decides` marks the rule a refusal is read against (`_firingDecision`). */
var _DEFAULT_ARMS = [
  /* A request to the person's own attacker site, which they configured as a server they run: it reaches no
     other party's state, so it fires at every other row's value, a live verify's attacker page among them. */
  { when: [{ signal: "reaches", values: ["attacker-site"] }],
    why: "the request reaches the attacker site the person configured as their own server, and no other " +
         "party's state" },
  /* The load a person asked for: the seed, and this zone's repeat of a request the person's browser was
     observed making, each `actor=tool` by a literal at its caller, in a document reached the same way. It
     roots a session: without it nothing is loaded. A drive's request reads `drive` and a page's `page`, so
     this arm admits neither. */
  { when: [{ signal: "actor", values: ["tool"] },
           { signal: "provenance", values: ["observed"] },
           { signal: "doc-reach", values: ["observed"] }],
    why: "the person named this address, or their own browser made exactly this request, in a document it " +
         "navigated to — so loading it is this browser being a browser" },
  /* The rule. Each conjunct refuses one population: `provenance` a request on a path that stood on a
     contradicted arm; `doc-reach` every request of a document such a path navigated to, whose own paths the
     engine grades from that document's start; `actor` a probe this zone composed and a request a drive
     minted; `witness` an address resting on a value the engine pinned, which a `derived` path can hold.
     The order is the one a refusal names a failed condition in.
     `derived` is the page's code computing from real inputs, and a document the unforced run navigated to
     is reached `derived`, so both words belong to the unforced run. Never drop a conjunct: each one alone
     passes every assert in this file. */
  { decides: true,
    when: [{ signal: "provenance", values: ["observed", "derived"] },
           { signal: "doc-reach", values: ["observed", "derived"] },
           { signal: "actor", values: ["page"] },
           { signal: "witness", values: ["unpinned"] }],
    why: "the analysed page's own code made this request on its own unforced run, from a document that run " +
         "reached, with an address resting on nothing this engine chose — what a browser loading the page " +
         "sends" }
];
var _EXPLORED = Object.create(null);
/* The per-site settings the same table holds beside the signals, keyed by a site's serialization (HTML
   §7.1.1.1 "obtain a site", which is an origin string): a row a person sets for one site, which no firing
   question reads. A row holds at most one value. */
var _SITE_SETTINGS = [{ name: "third-party-cookies", values: ["allow", "block"] }];
/* `_EXPLORED` is the per-origin egress table, origin → signal (or site setting) → value → true. Whether a
   host has stated it is a separate field, because an unstated table is empty and an empty one reads as
   policy. */
var _EXPLORED_STATED = false;
/* A host's promise to state the table. A host whose grants are in IndexedDB cannot state them until a read
   returns, and a request in that window may be answered neither from the empty table nor by stating one
   (which the restore would then replace), so `safeFetch` awaits this promise. The promise is the host's:
   this file reads no store and invents no default. `null` means no promise was made; the native host states
   synchronously, and a host that does neither still reaches the DCHECK in `_firingRefusal`. */
var _EXPLORED_STATING = null;
/* Whether an origin can be permitted at all, answering the reason it cannot or null. Two callers, two
   outcomes: `safeFetchPermit` aborts, because a caller inside this project passing a bad origin is broken;
   a surface with a person in front of it refuses with the reason, because an opaque origin is an ordinary
   state of the web and a fatal there would give any sandboxed page an abort switch on the trusted zone.
   The value must be its own serialized origin (parsed, re-serialized, compared), because `_isRealOrigin`
   only tests for "://" and a full address would be a key no request's `.origin` ever equals: a grant that
   reads as granted and matches nothing. `ws:`, `wss:` and `ftp:` origins serialize back to themselves, so
   the http(s) test is what refuses them; this file refuses those schemes before the wire anyway. `null` does
   not parse, so an opaque origin is refused by the parse. */
function safeFetchWidenable(origin) {
  var normalized = null;
  try { normalized = new URL(String(origin)).origin; } catch (e) { RETHROW_FATAL(e); normalized = null; }
  if (!_isRealOrigin(origin) || normalized !== origin) return "not-an-origin";
  if (origin.indexOf("http://") !== 0 && origin.indexOf("https://") !== 0) return "not-http";
  return null;
}
/* Whether `value` is one of the signal's values no setting permits (its `never` list). */
function _signalNever(s, value) {
  return Array.isArray(s.never) && s.never.indexOf(value) >= 0;
}
/* Whether `value` is one of the signal's values only a default arm admits (its `defaultOnly` list). */
function _signalDefaultOnly(s, value) {
  return Array.isArray(s.defaultOnly) && s.defaultOnly.indexOf(value) >= 0;
}
/* Whether a signal name and value are ones this file knows and a grant may name, as a reason or null, split
   for the same two callers as `safeFetchWidenable`. */
function safeFetchSignalUsable(signal, value) {
  var i;
  for (i = 0; i < _SIGNALS.length; i++) {
    if (_SIGNALS[i].name !== signal) continue;
    if (!_SIGNALS[i].gates) return "does-not-gate";
    if (_SIGNALS[i].values.indexOf(value) < 0) return "not-a-value";
    if (_signalNever(_SIGNALS[i], value)) return "never-permitted";
    return _signalDefaultOnly(_SIGNALS[i], value) ? "default-only" : null;
  }
  return "not-a-signal";
}
/* Whether a row name and value are a signal's or a site setting's this file knows, as a reason or null. */
function _tableRowUsable(name, value) {
  for (var i = 0; i < _SITE_SETTINGS.length; i++)
    if (_SITE_SETTINGS[i].name === name) return _SITE_SETTINGS[i].values.indexOf(value) >= 0 ? null : "not-a-value";
  return safeFetchSignalUsable(name, value);
}
function _isSiteSetting(name) {
  return _SITE_SETTINGS.some(function (t) { return t.name === name; });
}
/* The registry for a surface that renders one row per signal: name, grade and value space, copied. The `of`
   functions stay here, so a surface cannot compute a value the walk did not decide from. */
function safeFetchSignals() {
  return _SIGNALS.map(function (s) {
    return { name: s.name, gates: s.gates, certainty: s.certainty, values: s.values.slice(),
             never: Array.isArray(s.never) ? s.never.slice() : [],
             defaultOnly: Array.isArray(s.defaultOnly) ? s.defaultOnly.slice() : [] };
  });
}
/* A host's promise that it will state the table (see `_EXPLORED_STATING`), registered once at the host's
   load. `safeFetch` awaits it before the firing question, so the guarantee belongs to the chokepoint rather
   than to whichever door a request came through. A host that already holds its answer calls
   `safeFetchEgressStated` instead; this hands over the read itself, not a placeholder. */
function safeFetchEgressStating(promise) {
  CHECK(promise !== null && typeof promise === "object" && typeof promise.then === "function",
        "a host registered something other than a thenable as its promise to state the per-origin egress " +
        "table — every request this zone makes waits on it before the firing question is asked, so a value " +
        "with no `then` would be awaited once, settle immediately, and hand the walk the empty table this " +
        "promise exists to keep it away from");
  CHECK(!_EXPLORED_STATED,
        "a host promised to state its per-origin egress table after it had already stated one — the promise " +
        "is what a request waits on BEFORE the table exists, and one registered afterwards can only be a " +
        "second statement in flight, which REPLACES rather than adds and would revoke every grant a surface " +
        "made in between");
  CHECK(_EXPLORED_STATING === null,
        "a host promised twice to state its per-origin egress table — a waiting request awaits ONE promise, " +
        "so a second would leave which table that request is answered from decided by which registration " +
        "happened to win");
  _EXPLORED_STATING = promise;
}
/* The default arms, for a surface explaining why an app's scripts load before anything is permitted; a
   surface that explained it in its own words would be a second copy of the list. */
function safeFetchDefaultArms() {
  return _DEFAULT_ARMS.map(function (a) {
    return { when: a.when.map(function (c) { return { signal: c.signal, values: c.values.slice() }; }), why: a.why };
  });
}
/* The host states the table once, before anything may ask it: restored grants, or the empty table as the
   positive "nobody has permitted anything" (the native host, whose command line is a sentence for one run).
   A second call aborts, since it would replace grants a surface added since; later grants go through
   `safeFetchPermit`.
   The shape is `{ origin: { signal: [values] } }`. An origin whose entry is not an object is a legacy
   single-switch grant: carrying it forward would permit every value of every signal, including signals that
   did not exist when it was granted, so it is dropped and named in the answer. An origin with any signal
   whose values are not an array is dropped and named the same way, whole, so what the answer names is what
   is not permitted. A signal a stored grant does not name is not permitted, so adding a signal narrows every
   existing grant rather than silently widening it. */
function safeFetchEgressStated(table) {
  var origin, signal, values, why, i, legacy = [];
  CHECK(!_EXPLORED_STATED,
        "a host stated its per-origin egress table a second time — the table is the person's standing " +
        "sentence and a re-statement REPLACES it, so a grant made from a surface between the two calls is " +
        "revoked with nothing anywhere saying so. State it ONCE — synchronously before this zone can run, or " +
        "through the read registered with safeFetchEgressStating, which every request awaits — and add to it " +
        "with safeFetchPermit");
  _signalRegistryCheck();
  CHECK(table !== null && typeof table === "object" && !Array.isArray(table),
        "a host stated its per-origin egress table as " + JSON.stringify(table) + " rather than as an " +
        "origin-keyed object — an ARRAY is the shape the previous single-switch control persisted and it " +
        "cannot be read as this one, because a bare origin meant `permit everything` including the signals " +
        "that did not exist when it was written. This is the whole of what the chokepoint knows about what " +
        "a person has permitted, and a value it cannot walk would leave the table empty while the host " +
        "believed it had spoken");
  for (origin in table) {
    if (!Object.prototype.hasOwnProperty.call(table, origin)) continue;
    why = safeFetchWidenable(origin);
    CHECK(why === null,
          "a host stated " + JSON.stringify(origin) + " as a permitted origin and it is not one (" + why +
          ") — the entry would sit in this table matching no request's `.origin` for ever, which is a " +
          "permission that was granted, reads as granted, and refuses nothing. A host is the only writer of " +
          "its own store, so this is that store corrupted rather than a person mistyping");
    if (typeof table[origin] !== "object" || table[origin] === null || Array.isArray(table[origin]) ||
        Object.keys(table[origin]).some(function (k) { return !Array.isArray(table[origin][k]); })) {
      legacy.push(origin);
      continue;
    }
    for (signal in table[origin]) {
      if (!Object.prototype.hasOwnProperty.call(table[origin], signal)) continue;
      values = table[origin][signal];
      for (i = 0; i < values.length; i++) {
        /* A stored value this build no longer knows is dropped, not asserted: a renamed signal or a retired
           value is an upgrade, an unknown value permits nothing, and aborting would brick every profile that
           ever granted anything. This is the one place this file reads its own store as input. */
        if (_tableRowUsable(signal, values[i]) !== null) continue;
        if (_isSiteSetting(signal) && values.length !== 1) continue;
        if (!_EXPLORED[origin]) _EXPLORED[origin] = Object.create(null);
        if (!_EXPLORED[origin][signal]) _EXPLORED[origin][signal] = Object.create(null);
        _EXPLORED[origin][signal][values[i]] = true;
      }
    }
  }
  _EXPLORED_STATED = true;
  return legacy;
}
/* One signal, one value, at one origin: the door a per-signal surface writes through, deliberately not a
   batch, so what a person reads back is the table in force. `allow` false removes the value, because a
   permission with no withdrawal is a one-way door. */
function safeFetchPermit(origin, signal, value, allow) {
  var why;
  CHECK(_EXPLORED_STATED,
        "an egress permission was changed before this host stated its table — the statement is what restores " +
        "the person's earlier grants, so a grant that ran first would be REPLACED by that restore and the " +
        "person would watch a permission they had just made disappear");
  why = safeFetchWidenable(origin);
  CHECK(why === null,
        "an egress permission was asked for at " + JSON.stringify(origin) + ", which cannot be permitted (" +
        why + ") — the table is compared against a request URL's own `origin`, so a full address, an opaque " +
        "`null`, an explicit default port, a non-http(s) scheme or an empty string would sit here matching " +
        "nothing while reading as a permission somebody granted. A surface with a person in front of it asks " +
        "`safeFetchWidenable` and tells them why; a caller inside this project passing one is broken");
  why = safeFetchSignalUsable(signal, value);
  CHECK(why === null,
        "an egress permission was asked for the signal " + JSON.stringify(signal) + " at the value " +
        JSON.stringify(value) + ", which this file cannot use (" + why + ") — a permission naming a signal " +
        "or a value the firing walk never asks about is one that reads as granted and refuses nothing, and " +
        "a surface asks `safeFetchSignalUsable` before it offers a person the row");
  if (allow) {
    if (!_EXPLORED[origin]) _EXPLORED[origin] = Object.create(null);
    if (!_EXPLORED[origin][signal]) _EXPLORED[origin][signal] = Object.create(null);
    _EXPLORED[origin][signal][value] = true;
    return;
  }
  if (_EXPLORED[origin] && _EXPLORED[origin][signal]) delete _EXPLORED[origin][signal][value];
}
/* Permit every value of every gating signal at one origin: the coarse sentence, meant by
   `engine/trusted.mjs`'s `--explore <origin>` and by the popup's one-click allow. It is derived from the
   registry, so a signal added later is included. That is the opposite of the stored-grant rule above, and
   deliberately: a grant made now through a control that says "everything" does mean the new signal. */
function safeFetchWiden(origin) {
  var i, j;
  for (i = 0; i < _SIGNALS.length; i++) {
    if (!_SIGNALS[i].gates) continue;
    for (j = 0; j < _SIGNALS[i].values.length; j++)
      if (safeFetchSignalUsable(_SIGNALS[i].name, _SIGNALS[i].values[j]) === null)
        safeFetchPermit(origin, _SIGNALS[i].name, _SIGNALS[i].values[j], true);
  }
}
/* The person takes a whole origin's egress back; a site setting stored under the same key stays. Answers
   whether anything was there, so a surface reports what it did rather than a revocation of nothing. */
function safeFetchUnwiden(origin) {
  CHECK(_EXPLORED_STATED,
        "an egress permission was revoked before this host stated its table — the revocation would be " +
        "undone by the restore that follows it, and the person would watch a permission they had just " +
        "withdrawn come back");
  var had = false, name;
  for (name in _EXPLORED[origin] || {}) {
    if (_isSiteSetting(name)) continue;
    had = true;
    delete _EXPLORED[origin][name];
  }
  if (_EXPLORED[origin] && Object.keys(_EXPLORED[origin]).length === 0) delete _EXPLORED[origin];
  return had;
}
/* The gating signals an origin with any grant permits no value of: a signal added after its grant was stored
   (which a stored grant does not name, so it narrows), or one the person unticked entirely. Either way every
   request there is refused at that row, and the surface says it needs permitting again. */
function safeFetchEgressUnnamed(origin) {
  CHECK(_EXPLORED_STATED, "an origin's unnamed signals were read before this host stated its table");
  var e = _EXPLORED[origin], out = [], i;
  if (!e || !Object.keys(e).some(function (name) { return !_isSiteSetting(name); })) return out;
  for (i = 0; i < _SIGNALS.length; i++)
    if (_SIGNALS[i].gates && !(e[_SIGNALS[i].name] && Object.keys(e[_SIGNALS[i].name]).length > 0))
      out.push(_SIGNALS[i].name);
  return out;
}
/* The permitted origins, for a caller that asserts their absence where its own reasoning relies on it. */
function safeFetchWidenedOrigins() {
  CHECK(_EXPLORED_STATED,
        "the permitted-origin list was read before this host stated its table — an empty answer would be " +
        "read as `nobody has permitted anything`, which is a statement this host has not yet made, and a " +
        "surface rendering it would tell a person their standing grants are gone");
  return Object.keys(_EXPLORED).filter(function (o) {
    return Object.keys(_EXPLORED[o]).some(function (name) { return !_isSiteSetting(name); });
  });
}
/* The table in the shape `safeFetchEgressStated` takes, so a host persists what the chokepoint uses; the
   round trip is the contract. */
function safeFetchEgressTable() {
  var out = Object.create(null), origin, signal;
  CHECK(_EXPLORED_STATED,
        "the egress table was read before this host stated it — an empty answer would be persisted as `this " +
        "person has permitted nothing`, which would write over every standing grant with a statement this " +
        "host had not yet made");
  for (origin in _EXPLORED) {
    if (!Object.prototype.hasOwnProperty.call(_EXPLORED, origin)) continue;
    out[origin] = Object.create(null);
    for (signal in _EXPLORED[origin]) {
      if (!Object.prototype.hasOwnProperty.call(_EXPLORED[origin], signal)) continue;
      out[origin][signal] = Object.keys(_EXPLORED[origin][signal]);
    }
  }
  return out;
}
/* A site's setting `name` (a `_SITE_SETTINGS` row): its value, or null where the person set none. `site` is
   a site's serialization, as lib/site.js's siteOf gives it. */
function safeFetchSiteSetting(site, name) {
  CHECK(_EXPLORED_STATED, "a site setting was read before this host stated its table");
  CHECK(_isSiteSetting(name) && safeFetchWidenable(site) === null,
        "a site setting was asked as " + JSON.stringify(name) + " of " + JSON.stringify(site) + ", which is not " +
        "a site setting of a site");
  var row = _EXPLORED[site] && _EXPLORED[site][name];
  var values = row ? Object.keys(row) : [];
  DCHECK(values.length <= 1, "the site setting " + name + " of " + site + " holds two values");
  return values.length ? values[0] : null;
}
/* The person sets a site's setting to `value`, or clears it with null; at most one value stays. */
function safeFetchSetSiteSetting(site, name, value) {
  CHECK(_EXPLORED_STATED, "a site setting was changed before this host stated its table, whose restore would undo it");
  CHECK(safeFetchWidenable(site) === null && _isSiteSetting(name) &&
        (value === null || _tableRowUsable(name, value) === null),
        "a site setting was asked to be " + JSON.stringify(value) + " for " + JSON.stringify(name) + " of " +
        JSON.stringify(site) + ", which this table cannot hold");
  if (!_EXPLORED[site]) _EXPLORED[site] = Object.create(null);
  _EXPLORED[site][name] = Object.create(null);
  if (value !== null) _EXPLORED[site][name][value] = true;
  else delete _EXPLORED[site][name];
  if (Object.keys(_EXPLORED[site]).length === 0) delete _EXPLORED[site];
}
/* A promise that the table is stated, for a reader outside `safeFetch`'s own wait. */
function safeFetchEgressReady() {
  if (_EXPLORED_STATED) return Promise.resolve();
  CHECK(_EXPLORED_STATING !== null, "the egress table was awaited in a host that neither stated nor promised it");
  return _EXPLORED_STATING;
}
/* One origin's permissions row by row, with `false` for an unpermitted value rather than an absent key,
   because a checkbox rendered from a missing key looks like a denial and is a different fact. */
function safeFetchPermitted(origin) {
  var out = [], i, j, row;
  CHECK(_EXPLORED_STATED,
        "one origin's egress permissions were read before this host stated its table — every row would " +
        "answer `not permitted`, which is a sentence this host has not yet earned the right to say");
  for (i = 0; i < _SIGNALS.length; i++) {
    row = { name: _SIGNALS[i].name, gates: _SIGNALS[i].gates, certainty: _SIGNALS[i].certainty, values: [] };
    for (j = 0; j < _SIGNALS[i].values.length; j++)
      row.values.push({ value: _SIGNALS[i].values[j],
                        permitted: !!(_EXPLORED[origin] && _EXPLORED[origin][_SIGNALS[i].name] &&
                                      _EXPLORED[origin][_SIGNALS[i].name][_SIGNALS[i].values[j]] === true) });
    out.push(row);
  }
  return out;
}
/* The firing decision, answering "signal=value" for the fact that refused, or null to fire. A default arm
   whose every condition holds fires first; otherwise every gating signal's value must be permitted at the
   request URL's origin.
   It answers a signal and value, never a score, so a person reading a frontier that will not drain is told
   which fact held it. A permission changes which act may be spent, never what a reply is worth: `forced`
   still marks a reply as evidence about a request no client makes. Wherever this refuses, the address is
   still derived in full and reported. */
/* The first default arm that admits the vector `v`, or -1. */
function _defaultArmAdmitting(v) {
  return _DEFAULT_ARMS.findIndex(function (a) {
    return a.when.every(function (c) { return c.values.indexOf(v[c.signal]) >= 0; });
  });
}
function _firingRefusal(facts) {
  return _firingDecision(facts).refusal;
}
/* One firing question's whole answer: the vector it was judged on, what admitted it, and the refusal.
   `admittedBy` is {kind: "default", arm, why} for a default arm (the page's own act), {kind: "widened", origin}
   for a request that fires only because a person widened its origin (this tool's act: a forked value, a probe,
   a tool-composed request), and null beside a refusal. safeFetch writes a third on its entry, {kind: "record",
   refused}, for a refused request it served from the record of the unforced run's identical send. */
function _firingDecision(facts) {
  var v, i, s, arm, unpermitted = [], failed;
  v = _signalVector(facts);
  /* No pairing of `provenance` with `doc-reach` is asserted. `observed` with a `forced` reach is the tempting
     one to forbid, and it is exactly a page this tool chose to open making its own `fetch()`, which the
     engine correctly grades `observed`. All nine pairs are reachable and distinct: the request's grade is the
     engine's, the reach grade is this zone's, and neither bounds the other. */
  /* A value no setting permits refuses before any arm is asked, so no arm's silence about its row admits it. */
  for (i = 0; i < _SIGNALS.length; i++)
    if (_signalNever(_SIGNALS[i], v[_SIGNALS[i].name]))
      return { vector: v, admittedBy: null, refusal: _SIGNALS[i].name + "=" + v[_SIGNALS[i].name] };
  /* Defaults first: they are permissions and the table only widens, so the order changes no answer. */
  arm = _defaultArmAdmitting(v);
  if (arm >= 0)
    return { vector: v, admittedBy: { kind: "default", arm: arm, why: _DEFAULT_ARMS[arm].why }, refusal: null };
  /* An unstated table is empty, so the walk would refuse a permitted origin in the policy's own voice. That
     is conservative, hence a DCHECK, and unreadable, hence asserted at all. `safeFetch` awaits
     `_EXPLORED_STATING` first, so this checks that construction; a host that neither states nor promises
     still reaches it. The table is read for every request no default arm admits, including plain data GETs
     the offscreen composes with no engine, so a statement placed at a per-document door could not close
     this window. */
  DCHECK(_EXPLORED_STATED,
         "the firing question was asked before this host STATED its per-origin egress table — the table is " +
         "empty until a host speaks, so this refusal would tell a person their own standing permission does " +
         "not exist, in the policy's own voice and indistinguishable from a policy they set. A host either " +
         "STATES the table before this zone can run (the native host, before it reads `--explore`) or " +
         "REGISTERS the read that will state it (`safeFetchEgressStating`, which every request awaits), so " +
         "reaching here unstated is a host that did NEITHER — not a door that was never told");
  for (i = 0; i < _SIGNALS.length; i++) {
    s = _SIGNALS[i];
    if (!s.gates) continue;
    if (_EXPLORED[facts.url.origin] && _EXPLORED[facts.url.origin][s.name] &&
        _EXPLORED[facts.url.origin][s.name][v[s.name]] === true) continue;
    unpermitted.push(s.name);
  }
  if (unpermitted.length === 0)
    return { vector: v, admittedBy: { kind: "widened", origin: facts.url.origin }, refusal: null };
  /* The refusal names the fact that decided it: the first condition of the deciding rule this request fails
     whose value the origin does not permit either, so a forced request at an origin nobody widened reads
     `provenance=forced`. Where the person permitted every such fact, a row they have not answered holds the
     request, and the first of those in registry order is named. */
  failed = _DEFAULT_ARMS.find(function (a) { return a.decides === true; }).when.find(function (c) {
    return c.values.indexOf(v[c.signal]) < 0 && unpermitted.indexOf(c.signal) >= 0;
  });
  s = failed === undefined ? unpermitted[0] : failed.signal;
  return { vector: v, admittedBy: null, refusal: s + "=" + v[s] };
}
/* The zone's count of the entries its reply records carried, keyed "<how> <what admitted it>": `default`,
   `widened` or `record` for a judged request, `none` for a refused one, `cache` for a hop the HTTP cache
   answered. */
var _EGRESS_CENSUS = Object.create(null);
function safeFetchEgressCensus() {
  return Object.assign({}, _EGRESS_CENSUS);
}
/* The same answer for a caller whose act is not a fetch: `bridge.js`'s route-declaration arm, which records a
   work item loaded rounds later, and `engine/trusted.mjs`'s `navigate`, which must decline rather than
   produce the empty Document a network refusal would. It answers the signal that refused, so a host can say
   why without re-deriving the policy; the sentence around it is the host's own. It takes an absolute URL
   (one that will not parse throws: that is a caller's serializer disagreeing with a URL parser) and a facts
   object rather than positional arguments, so a new signal is a key and not a shifted operand. Every fact is
   required, because a hypothetical answered from fewer facts is about a different request. */
function safeFetchFiringRefusal(facts) {
  CHECK(facts !== null && typeof facts === "object" && typeof facts.url === "string",
        "safeFetchFiringRefusal was asked without an absolute URL — the origin comparison is this file's to " +
        "make and a caller that parsed one for itself would be the second copy of that rule");
  return _firingRefusal({ url: new URL(String(facts.url)), destination: facts.destination,
                          provenance: facts.provenance, pinned: facts.pinned, doc: facts.doc,
                          actor: facts.actor, driven: facts.driven, address: facts.address,
                          bodyClass: facts.bodyClass,
                          method: facts.method, credentialed: facts.credentialed, headers: facts.headers });
}
/* A cross-document act reaches another document's state, so it is judged by `_firingRefusal` over the facts a
   fetch states, with no arm of its own: a message routed to another instance (HTML §9.3.3 "Posting messages")
   and a cross-agent operation (a member of another agent's Window, §7.2.1; a traversable's close, §7.3.1.6; a
   lent object's internal method). Which member a read names changes no signal, and a member outside §7.2.1.3.1
   CrossOriginProperties is never routed (window_proxy.c answers it locally), so there is no branch on it.
   It carries no address, so its URL is the receiving document's origin, which this zone composed (`address`
   reads `concrete`), and `url-authority` reads `unknown`.
   An opaque origin is one no widening can name, so it is asked as `about:blank`'s, whose origin is opaque too.
   Its destination is the empty string, a value like a page `fetch()`, and it is judged as a GET: a
   delivery or a read sends no verb. It is the page's act. Its provenance,
   drive and witness are the sending timeline's marks (the record's `<derived|forced>,<driven|undriven>,
   <pinned|unpinned>` word, solver/engine.c's engine_record_head). `doc` is the sending document's record. It
   pays no cookie jar and carries no header list. Answers `_firingRefusal`'s "signal=value", or null to
   deliver. */
function safeFetchCrossDocumentRefusal(act) {
  var m;
  CHECK(act !== null && typeof act === "object" && typeof act.targetOrigin === "string" &&
        typeof act.marks === "string" && act.doc !== null && typeof act.doc === "object",
        "a cross-document act was judged without the receiving instance's origin, the record's marks or the " +
        "acting document's record");
  m = act.marks.split(",");
  CHECK(m.length === 3 && (m[0] === "derived" || m[0] === "forced") &&
        (m[1] === "driven" || m[1] === "undriven") && (m[2] === "pinned" || m[2] === "unpinned"),
        "a cross-document act's marks " + JSON.stringify(act.marks) + " are not the word " +
        "engine_record_head writes — the engine that routed it refuses any other (peer_marks_word_ok)");
  return _firingRefusal({ url: new URL(_isRealOrigin(act.targetOrigin) ? act.targetOrigin + "/" : "about:blank"),
                          destination: "", provenance: m[0], pinned: m[2], doc: act.doc, actor: "page",
                          driven: m[1], address: "concrete", bodyClass: "none", method: "GET",
                          credentialed: false, headers: null });
}
/* The vector for a hypothetical request, for a surface that shows a person what they are deciding about. It
   is the same derivation the firing walk reads, so the rows a person ticks are about the request the policy
   sees. */
function safeFetchSignalVector(facts) {
  CHECK(facts !== null && typeof facts === "object" && typeof facts.url === "string",
        "the signal vector was asked for without an absolute URL — `url-authority` is read off the parsed " +
        "address, so a caller with none would be shown a row about nothing");
  /* Every fact is forwarded, `doc` included; a field dropped here would compute outside its row's value
     space and render a row about nothing. */
  return _signalVector({ url: new URL(String(facts.url)), destination: facts.destination,
                         provenance: facts.provenance, pinned: facts.pinned, doc: facts.doc,
                         actor: facts.actor, driven: facts.driven, address: facts.address,
                         bodyClass: facts.bodyClass,
                         method: facts.method, credentialed: facts.credentialed, headers: facts.headers });
}
/* Fetch §4.10 "CORS check", over the reply's own headers: "Let origin be the result of getting
   `Access-Control-Allow-Origin` from response's header list. If origin is null, then return failure. Null is not
   `null`. If request's credentials mode is not "include" and origin is `*`, then return success. If the result of
   byte-serializing a request origin with request is not origin, then return failure. If request's credentials
   mode is not "include", then return success. Let credentials be the result of getting
   `Access-Control-Allow-Credentials` from response's header list. If credentials is `true`, then return
   success. Return failure." Answers the failing step's name, or null for success. `headers` is this file's
   lowercased map, whose duplicate values are already combined, so a list of two origins matches neither. */
function _corsCheck(headers, serializedOrigin, credentialsMode) {
  var origin = headers["access-control-allow-origin"];
  if (origin === undefined) return "no-allow-origin";
  if (credentialsMode !== "include" && origin === "*") return null;
  if (origin !== serializedOrigin) return "allow-origin-mismatch";
  if (credentialsMode !== "include") return null;
  if (headers["access-control-allow-credentials"] === "true") return null;
  return "no-allow-credentials";
}
/* Fetch §2.2.6 "Responses"' CORS filtered response: "a filtered response whose type is "cors" and header list
   excludes any headers in internal response's header list whose name is not a CORS-safelisted response-header
   name, given internal response's CORS-exposed header-name list". §2.2.2 "Headers" defines the safelist
   ("`Cache-Control`, `Content-Language`, `Content-Length`, `Content-Type`, `Expires`, `Last-Modified`,
   `Pragma`, Any item in list that is not a forbidden response-header name") and the forbidden names
   (`Set-Cookie`, `Set-Cookie2`). The exposed list is `Access-Control-Expose-Headers`, whose `*` counts as a
   wildcard only for a request whose credentials mode is not "include". The filter runs here, in the zone that
   read the headers, so what the page may not read never reaches the engine. */
var _CORS_SAFELISTED_RESPONSE = ["cache-control", "content-language", "content-length", "content-type",
                                 "expires", "last-modified", "pragma"];
var _FORBIDDEN_RESPONSE = ["set-cookie", "set-cookie2"];
function _corsFilteredHeaders(headers, credentialsMode) {
  var exposed = String(headers["access-control-expose-headers"] === undefined ? ""
                       : headers["access-control-expose-headers"])
    .split(",").map(function (n) { return n.trim().toLowerCase(); }).filter(function (n) { return n !== ""; });
  var all = credentialsMode !== "include" && exposed.indexOf("*") >= 0;
  var out = {}, k;
  for (k in headers) {
    if (!Object.prototype.hasOwnProperty.call(headers, k)) continue;
    if (_CORS_SAFELISTED_RESPONSE.indexOf(k) >= 0 ||
        (_FORBIDDEN_RESPONSE.indexOf(k) < 0 && (all || exposed.indexOf(k) >= 0)))
      out[k] = headers[k];
  }
  return out;
}
/* Fetch §2.2.1 "Methods": "A CORS-safelisted method is a method that is `GET`, `HEAD`, or `POST`." */
function _corsSafelistedMethod(m) { return m === "GET" || m === "HEAD" || m === "POST"; }
/* Fetch §2.2.2 "Headers"' CORS-unsafe request-header byte: below 0x20 except HT, or one of `"():<>?@[\]{}`
   and DEL. A header value here is a byte string, one code point per byte. */
function _hasCorsUnsafeRequestHeaderByte(v) {
  for (var i = 0; i < v.length; i++) {
    var c = v.charCodeAt(i);
    if ((c < 0x20 && c !== 0x09) || '"():<>?@[\\]{}\x7f'.indexOf(v[i]) >= 0) return true;
  }
  return false;
}
/* MIME Sniffing §4.4 "Parsing a MIME type", as far as the essence: a parameter never makes that algorithm fail,
   so the essence (or null for failure) is decided by its steps before the parameters. */
var _HTTP_TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
function _mimeEssence(input) {
  var s = input.replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, "");
  var slash = s.indexOf("/");
  if (slash < 0) return null;
  var rest = s.slice(slash + 1), semi = rest.indexOf(";");
  var type = s.slice(0, slash), sub = (semi < 0 ? rest : rest.slice(0, semi)).replace(/[\t\n\r ]+$/, "");
  if (!_HTTP_TOKEN.test(type) || !_HTTP_TOKEN.test(sub)) return null;
  return (type + "/" + sub).toLowerCase();
}
/* Fetch §2.2.2's "parse a single range header value" with allowWhitespace false: [start, end] with null for an
   absent bound, or null for failure. */
function _singleRangeValue(v) {
  var m = /^bytes=([0-9]*)-([0-9]*)$/.exec(v);
  if (!m) return null;
  var start = m[1] === "" ? null : Number(m[1]), end = m[2] === "" ? null : Number(m[2]);
  if (start === null && end === null) return null;
  if (start !== null && end !== null && start > end) return null;
  return [start, end];
}
/* Fetch §2.2.2's CORS-safelisted request-header, given a name and a value. */
function _corsSafelistedRequestHeader(name, value) {
  if (value.length > 128) return false;
  switch (name.toLowerCase()) {
  case "accept":
    return !_hasCorsUnsafeRequestHeaderByte(value);
  case "accept-language":
  case "content-language":
    return /^[0-9A-Za-z *,\-.;=]*$/.test(value);
  case "content-type": {
    if (_hasCorsUnsafeRequestHeaderByte(value)) return false;
    var essence = _mimeEssence(value);
    return essence === "application/x-www-form-urlencoded" || essence === "multipart/form-data" ||
           essence === "text/plain";
  }
  case "range": {
    var r = _singleRangeValue(value);
    return r !== null && r[0] !== null;
  }
  default:
    return false;
  }
}
/* Fetch §2.2.2's CORS-unsafe request-header names of a header list (`[name, value]` pairs), as the
   sorted-lowercase set the spec returns: every unsafelisted name, and every name once the safelisted values
   exceed 1024 bytes together. */
function _corsUnsafeRequestHeaderNames(list) {
  var unsafe = [], potentially = [], size = 0;
  list.forEach(function (h) {
    if (!_corsSafelistedRequestHeader(h[0], h[1])) unsafe.push(h[0]);
    else { potentially.push(h[0]); size += h[1].length; }
  });
  if (size > 1024) unsafe = unsafe.concat(potentially);
  var set = [];
  unsafe.forEach(function (n) { n = n.toLowerCase(); if (set.indexOf(n) < 0) set.push(n); });
  return set.sort();
}
/* Fetch §2.2.2's "extract header list values" for a `#method` or `#field-name` header (Fetch §3.3.4 "HTTP
   new-header syntax"): undefined where the header is absent (the spec's null), null where its value is not
   that list (the spec's failure), else the elements. `headers` is this file's lowercased map, whose repeated
   fields are already joined with ", ", which is the same list. */
function _extractTokenList(headers, name) {
  if (headers[name] === undefined) return undefined;
  var out = [], parts = String(headers[name]).split(",");
  for (var i = 0; i < parts.length; i++) {
    var e = parts[i].replace(/^[\t ]+|[\t ]+$/g, "");
    if (e === "") continue;
    if (!_HTTP_TOKEN.test(e)) return null;
    out.push(e);
  }
  return out;
}
/* Fetch §4.8 "CORS-preflight fetch" for a request (`method`, `href`, `headerList`, credentials mode, use-CORS-
   preflight flag), CORS-checked against `origin`, the serialized request origin. Answers null on success or
   the name of the step that failed, which is a network error. The preflight is uncredentialed and does not
   follow a redirect: a 3xx is not an ok status. This zone provides no CORS-preflight cache, so the step "If
   the user agent does not provide for a cache, then return response" ends it and `Access-Control-Max-Age` is
   not read. `speak` is the OPTIONS request's speak-as (`_speakAs`), and `send(init)` answers the Response of
   the wire the request's own send uses: safeFetch's coalesced `_wireSend`, or the operator tier's `_wire`. */
async function _corsPreflight(href, method, headerList, credentialsMode, usePreflightFlag, origin, signal, speak,
                              send) {
  var unsafe = _corsUnsafeRequestHeaderNames(headerList);
  /* `Access-Control-Request-Method` and `-Headers` are forbidden request-headers (§2.2.2), which a script's
     fetch cannot set; the OPTIONS headers speakAsPreflightHeaders composes (lib/speak-as.js) supply both. */
  CHECK(!!speak, "a CORS preflight was sent speaking as no document — Fetch §4.8's preflight carries the " +
        "request's origin, and safeFetch preflights only a request with an identity");
  DCHECK(typeof send === "function", "a CORS preflight was handed no wire — its caller states the one its request uses");
  var init = { method: "OPTIONS", headers: [["Accept", "*/*"]], credentials: "omit", redirect: "manual", speakAs: speak };
  if (signal) init.signal = signal;
  var resp = await send(init);
  /* A 3xx is not an ok status and is not followed. */
  var headers = {}, status = resp.status;
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
  if (resp.body) {
    try { await resp.body.cancel(); } catch (e) { RETHROW_FATAL(e); }
  }
  var cors = _corsCheck(headers, origin, credentialsMode);
  if (cors) return "cors-check:" + cors;
  if (!(status >= 200 && status <= 299)) return "status:" + status;
  var methods = _extractTokenList(headers, "access-control-allow-methods");
  var headerNames = _extractTokenList(headers, "access-control-allow-headers");
  if (methods === null) return "allow-methods-unparseable";
  if (headerNames === null) return "allow-headers-unparseable";
  if (methods === undefined && usePreflightFlag) methods = [method];
  methods = methods || [];
  headerNames = (headerNames || []).map(function (n) { return n.toLowerCase(); });
  if (methods.indexOf(method) < 0 && !_corsSafelistedMethod(method) &&
      (credentialsMode === "include" || methods.indexOf("*") < 0))
    return "method-not-allowed:" + method;
  for (var i = 0; i < headerList.length; i++)
    if (headerList[i][0].toLowerCase() === "authorization" && headerNames.indexOf("authorization") < 0)
      return "header-not-allowed:authorization";
  for (var j = 0; j < unsafe.length; j++)
    if (headerNames.indexOf(unsafe[j]) < 0 && (credentialsMode === "include" || headerNames.indexOf("*") < 0))
      return "header-not-allowed:" + unsafe[j];
  return null;
}
/* The facts and the header set ({name: value, or null for a field the rule removes}) of a send speaking as
   `identity` to the last URL of `urlList`, with its Cookie only where `credentialed`. The cache asks it
   before a send to compare Vary fields, and `_speakAs` asks it for the send itself. */
async function _speakAsRequest(identity, urlList, method, mode, destination, credentialed) {
  CHECK(typeof speakAsHeaders === "function",
        "a request states an identity on a host that loads no lib/speak-as.js — the identity's headers would " +
        "be dropped and the request would go out as this extension");
  CHECK(!!identity.client && typeof identity.client.docKey === "string",
        "a request speaks as facts with no document key — a speak-as rule is scoped to one document's sends");
  var facts = credentialed ? identity : Object.assign({}, identity, { credentials: "omit" });
  var cookie = await cookieHeaderFor(facts, urlList[urlList.length - 1]);
  return { facts: facts, headers: speakAsHeaders(facts, method, mode, destination, urlList, cookie) };
}
/* A request that speaks as an identity (lib/request-identity.js's identityFor answer) runs `send(speak)`, where
   `speak` is {request, preflight}: the speak-as ({docKey, origin, stated, headers}) its own wire send carries,
   and, when `preflightNames` is a list, its OPTIONS send's, for the last URL of `urlList` (Fetch §2.2.5's URL
   list, which a redirect grows). Its cookies are the
   identity's (cookieHeaderFor's Cookie) only where this zone is willing (`credentialed`); otherwise it carries
   none. Where it carries them and the request is the page's act or the person's (`jarTakesReply`), its
   reply's Set-Cookie is stored in the identity's jar (lib/cookie-store.js) before anything reads the reply;
   a reply to this tool's own act never reaches the person's jar, since a forced reply is never merged into
   observed state. The zone's own fetch omits credentials, so this extension's jar neither adds nor receives
   a cookie. `stated` is null, or the operator tier's Send-panel record (safeFetchStatedShape),
   whose Origin and Referer replace the identity's on every header set of the send. A request with no identity
   runs `send(null)` as this extension, uncredentialed. A reply's `requestHeaders` is the speak-as set its
   send carried ({} for none). */
async function _speakAs(identity, urlList, method, mode, destination, credentialed, jarTakesReply, preflightNames,
                        stated, init, send) {
  var href = urlList[urlList.length - 1];
  if (!identity) {
    var bare = await send(null);
    if (bare.resp) { bare.setCookies = []; bare.requestHeaders = {}; }
    return bare;
  }
  var composed = await _speakAsRequest(identity, urlList, method, mode, destination, credentialed);
  var facts = composed.facts;
  init.credentials = "omit";
  var rules = [{ method: method, headers: composed.headers }];
  if (preflightNames !== null)
    rules.push({ method: "OPTIONS", headers: speakAsPreflightHeaders(facts, method, destination, urlList, preflightNames) });
  if (stated !== null)
    rules.forEach(function (r) {
      if (stated.origin !== null) r.headers["Origin"] = stated.origin.header;
      if (stated.referer !== null) r.headers["Referer"] = stated.referer;
    });
  /* The worker admits an Origin or Referer outside the identity only as a value the person stated; removing
     a header needs no admission. */
  var admitted = stated === null ? null
    : { origin: stated.origin === null ? null : stated.origin.header, referer: stated.referer };
  var speakOf = function (r) {
    return { docKey: identity.client.docKey, origin: identity.origin, stated: admitted, headers: r.headers };
  };
  var sent = await send({ request: speakOf(rules[0]), preflight: rules.length > 1 ? speakOf(rules[1]) : null });
  if (!sent.resp) return sent;
  sent.requestHeaders = rules[0].headers;
  /* Fetch §4.7 "HTTP-network fetch": "If includeCredentials is true, then the user agent should parse and store
     response `Set-Cookie` headers", which cookieArmFor's "listed" arm is. The fields also ride the reply as
     [request URL, field] pairs (`setCookies`), which the asking flow's own jar applies (the engine's
     cookie_jar), whoever's act the request was; a field webRequest reported as bytes is text to neither. */
  var listed = cookieArmFor(facts, href).arm === "listed";
  var fields = listed ? _setCookiesOf(sent.resp) : [];
  sent.setCookies = fields.filter(function (f) { return f !== null; }).map(function (f) { return [href, f]; });
  if (listed && jarTakesReply) await cookieStoreSetCookies(fields, href, facts);
  return sent;
}
function _corbDeniesScript(mime, nosniff, sniff, sameOrigin) {
  /* CORB for a script-like load, over facts already computed, answering the rule that refused or null, so
     the status message names which of four rules fired. Same-origin: the page's own data is its to read, and
     only a protected non-JavaScript type reaching a code loader is refused. */
  if (sameOrigin)
    return _corbProtectedMime(mime) && !_jsMime(mime) ? "same-origin-protected" : null;
  if (_corbProtectedMime(mime)) return "protected-type";      // CORB-protected type
  if (nosniff && !_jsMime(mime)) return "nosniff-not-js";      // browser blocks too
  if (sniff.protected) return "sniffed-data";                  // mislabeled data
  return null;
}

/* The IP address space of a host, for the origin-relative SSRF rule: Local Network Access §2.1 "IP Address
   Space"'s "Non-public IP address blocks" table, row for row in `_v4Space` and `_ipAddressSpace`, including
   ::ffff:0:0/96's "see mapped IPv4 address"; every other address is `public`. Two names are classified by the
   standards that define them: `localhost` and `*.localhost` are loopback (RFC 6761 §6.3 "Domain Name
   Reservation Considerations for "localhost."": they "will always resolve to the respective IP loopback
   address"), and `*.local` is local (Local Network Access §2.5 "Mixed Content" names a `.local` domain as a
   request known to be a local network request). Any other name is `public` here; what it resolves to is not
   visible to this zone, which is the private-network-by-name residual in SECURITY.md.
   Every host comes from the WHATWG URL parser, so it is canonical: IPv4 is dotted-quad (the parser folds
   decimal, octal and hex spellings, so `http://2130706433/` is `127.0.0.1`), and IPv6 is the serializer's
   lowercase hex pieces with at most one `::` and no dotted tail. */
function _v4Octets(host) {
  var m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host);
  return m ? [+m[1], +m[2], +m[3], +m[4]] : null;
}
function _v6Pieces(host) {
  if (host.indexOf(":") < 0) return null;
  var halves = host.split("::");
  DCHECK(halves.length <= 2, "an IPv6 host carries two `::` — the WHATWG IPv6 serializer compresses one run");
  var head = halves[0] === "" ? [] : halves[0].split(":");
  var tail = halves.length === 2 ? (halves[1] === "" ? [] : halves[1].split(":")) : [];
  var fill = halves.length === 2 ? 8 - head.length - tail.length : 0;
  var parts = head.concat(new Array(fill).fill("0"), tail);
  DCHECK(parts.length === 8 && parts.every(function (p) { return /^[0-9a-f]{1,4}$/.test(p); }),
         "an IPv6 host is not eight hex pieces once expanded (" + host + ") — the URL parser serializes every " +
         "IPv6 address as hex pieces, so this host did not come from URL.hostname");
  return parts.map(function (p) { return parseInt(p, 16); });
}
function _v4Space(o) {
  if (o[0] === 127) return "loopback";
  if (o[0] === 10) return "local";
  if (o[0] === 100 && o[1] >= 64 && o[1] <= 127) return "local";
  if (o[0] === 172 && o[1] >= 16 && o[1] <= 31) return "local";
  if (o[0] === 192 && o[1] === 168) return "local";
  if (o[0] === 198 && (o[1] === 18 || o[1] === 19)) return "loopback";
  if (o[0] === 169 && o[1] === 254) return "local";
  if (o[0] === 0 && o[1] === 0 && o[2] === 0 && o[3] === 0) return "loopback";
  if (o[0] === 0) return "local";
  return "public";
}
function _ipAddressSpace(host) {
  if (!host) return "public";
  host = String(host).toLowerCase().replace(/^\[|\]$/g, "");
  /* The canonical form is the contract: a literal with both a colon and a dot is the dotted IPv4-in-IPv6
     text no URL parser emits, so its caller passed a raw string instead of `URL.hostname`. */
  DCHECK(!(host.indexOf(":") >= 0 && host.indexOf(".") >= 0),
         "a host reached the SSRF classifier as an IPv4-in-IPv6 literal with a dotted tail (" + host + ") — " +
         "the WHATWG IPv6 serializer emits hex pieces only, so this host did not come from URL.hostname and " +
         "is about to be classified by rules written for the form that one produces");
  if (host === "localhost" || host.endsWith(".localhost")) return "loopback";
  if (host.endsWith(".local")) return "local";
  var o = _v4Octets(host);
  if (o) return _v4Space(o);
  var p = _v6Pieces(host);
  if (!p) return "public";
  var zero5 = p[0] === 0 && p[1] === 0 && p[2] === 0 && p[3] === 0 && p[4] === 0;
  if (zero5 && p[5] === 0xffff)
    return _v4Space([p[6] >> 8, p[6] & 255, p[7] >> 8, p[7] & 255]);
  if (zero5 && p[5] === 0 && p[6] === 0 && (p[7] === 0 || p[7] === 1)) return "loopback";
  if ((p[0] & 0xfe00) === 0xfc00) return "local";
  if ((p[0] & 0xffc0) === 0xfe80) return "local";
  if ((p[0] & 0xffc0) === 0xfec0) return "local";
  if (p[0] === 0x2001 && p[1] === 0x0db8) return "local";
  if (p[0] === 0x3fff && (p[1] & 0xf000) === 0) return "local";
  return "public";
}
/* Local Network Access §2.1's "less public than": loopback is less public than local and public, and local is
   less public than public. A request is refused when its target is less public than the page principal, which
   is §2.2 "Local Network Request"'s definition of the request a browser would gate. The rank is
   lib/document-record.js's, whose records carry the page principal's space. */
function _lessPublicThan(lhs, rhs) { return IP_ADDRESS_SPACE_RANK[lhs] < IP_ADDRESS_SPACE_RANK[rhs]; }
/* The destructive-path deny list: the one place this project matches on a name, and it matches only to
   refuse. A wrong deny costs one unfired request that forced execution still derives and reports, while a
   wrong assert would fabricate a finding that propagates, so over-broad is its cheap direction.
   Forced execution builds requests no real client makes, and RFC 9110 §9.2.1 "Safe Methods" leaves keeping
   GET side-effect-free to the resource owner while naming the failure: "unfortunate side effects when
   automated processes perform a GET on every URI reference". A GET that ends the person's session
   mid-analysis is a CSRF this tool commits against its own user, so the list applies where the session is
   spent (credentialed, and not `observed`; see `safeFetch`). A path that does not match is not thereby safe:
   the list is a floor under the egress policy and under this tool's own autonomy, never a substitute.
   Matching is by whole token: each path segment and query token is tested raw and with `-`, `_` and `.`
   removed (`log-out` reaches `logout`; `/deleted-items` does not reach `delete`). Tokens are taken raw and
   after each percent-decoding pass, because a URL parser only encodes and a server treats `%6F` as `o` (RFC
   3986 §6.2.2.2 "Percent-Encoding Normalization"). Decoding reserved octets such as `%2F` goes beyond
   §6.2.2.2's normalization, which is safe here because an extra token can only refuse one more request. */
var _DESTRUCTIVE = [
  /* ending a session or revoking an authorization */
  "logout", "logoff", "signout", "signoff", "deauth", "deauthorize", "revoke",
  "endsession", "destroysession", "invalidate", "unsubscribe", "optout",
  /* destroying or disabling a resource or an account */
  "delete", "destroy", "remove", "purge", "erase", "wipe", "truncate",
  "deactivate", "disable", "terminate", "cancel", "reset",
  "unlink", "unfollow", "unfriend", "deleteaccount", "closeaccount"
];
/* An entry that could never match is a silent hole, so its shape is asserted: the matcher compares
   lowercased, separator-stripped tokens, and an entry with an uppercase letter or a separator would match
   nothing. */
var _DESTRUCTIVE_SET = (function () {
  var m = Object.create(null);
  for (var i = 0; i < _DESTRUCTIVE.length; i++) {
    DCHECK(/^[a-z0-9]+$/.test(_DESTRUCTIVE[i]),
           "a destructive-path token is compared against lowercased separator-stripped " +
           "tokens, so one carrying an uppercase letter or a separator can never match — " +
           "it would be a gate entry that looks protective and refuses nothing: " +
           _DESTRUCTIVE[i]);
    m[_DESTRUCTIVE[i]] = true;
  }
  return m;
})();
/* URL Standard §1.3 "Percent-encoded bytes"' percent-decode, one pass. It is that algorithm rather than
   `decodeURIComponent` because it cannot fail: a `%` not followed by two hex digits is data, where
   `decodeURIComponent` throws, and a catch in front of this gate would read "could not evaluate" as "no
   token matched". It stops at bytes: each triplet becomes one code unit with that byte's value, which is
   enough because the token class is ASCII and any other byte is a separator. Both hex cases are accepted,
   as RFC 3986 §2.1 "Percent-Encoding" makes them equivalent. */
function _percentDecode(s) {
  var out = "";
  for (var i = 0; i < s.length; i++) {
    var c = s.charAt(i);
    if (c !== "%" || i + 2 >= s.length || !/^[0-9a-fA-F]{2}$/.test(s.substr(i + 1, 2))) { out += c; continue; }
    out += String.fromCharCode(parseInt(s.substr(i + 1, 2), 16));
    i += 2;
  }
  return out;
}
/* One form's tokens, so the raw and decoded forms share one matcher. */
function _destructiveIn(form) {
  var parts = form.toLowerCase().split(/[^a-z0-9._-]+/);
  for (var i = 0; i < parts.length; i++) {
    var t = parts[i];
    if (!t) continue;
    if (_DESTRUCTIVE_SET[t]) return t;
    var squashed = t.replace(/[-._]/g, "");
    if (squashed && _DESTRUCTIVE_SET[squashed]) return squashed;
  }
  return "";
}
/* Answers the token that refused this URL, or "" as the positive statement that nothing matched. The token
   travels in the status message so a reviewer can dispute this entry rather than the list. The raw form is
   asked first, then each successive decoding, so the gate need not know how many times a server decodes.
   The argument is CHECKed rather than guarded: a non-URL composes a form that matches nothing and would
   permit a credentialed request in silence, so release must not proceed through it. URL Standard §6.1 "URL
   class" gives `pathname` and `search` no failure step, so there is nothing to catch. */
function _destructiveToken(u) {
  CHECK(!!u && typeof u.pathname === "string" && typeof u.search === "string",
        "the destructive-path deny list was handed something that is not a URL — this gate is the last thing " +
        "between a credentialed GET and a path that ends the person's session, it reads `pathname` and " +
        "`search` off a URL this zone itself parsed, and anything else composes a form that matches no token " +
        "and permits the request in silence");
  var form = u.pathname + "&" + u.search;
  for (;;) {
    var t = _destructiveIn(form);
    if (t) return t;
    var next = _percentDecode(form);
    /* Termination is structural, not a cap: a pass that changed anything replaced a three-unit triplet with
       one unit and is strictly shorter, and a pass that changed nothing returns the same string. Asserted
       because the exit rests on it. */
    DCHECK(next === form || next.length < form.length,
           "a percent-decoding pass returned a string that differs from its input yet is no shorter — URL " +
           "Standard §1.3 \"Percent-encoded bytes\" replaces a three-code-unit triplet with one byte and " +
           "copies every other code unit unchanged, so a change that does not shorten is this decoder no " +
           "longer implementing it, and the destructive-path loop uses that length to know it is done");
    if (next.length >= form.length) return "";
    form = next;
  }
}

/* Fetch §2.2.3 "Statuses"' redirect statuses. */
var _REDIRECT_STATUSES = [301, 302, 303, 307, 308];
/* The redirect a response is, as {status, headers}, or null for a response that is not one (a 3xx with no
   `Location` is Fetch §4.5 "HTTP-redirect fetch"'s "If locationURL is null, then return response"). Every send
   asks the wire for redirect "manual", so nothing follows a redirect but this zone, and the wire answers the
   3xx itself: webRequest's record of it (lib/wire.js), or a process's own fetch (engine/trusted.mjs). */
async function _redirectOf(resp) {
  DCHECK(resp.type !== "opaqueredirect",
         "the wire answered an opaque-redirect — lib/wire.js answers a 3xx with the record webRequest reported, and a " +
         "process's fetch answers the 3xx itself");
  if (_REDIRECT_STATUSES.indexOf(resp.status) < 0 || !resp.headers.has("location")) return null;
  var own = {};
  resp.headers.forEach(function (v, k) { own[String(k).toLowerCase()] = v; });
  if (resp.body) {
    try { await resp.body.cancel(); } catch (e) { RETHROW_FATAL(e); }
  }
  return { status: resp.status, headers: own };
}
/* Fetch §4.5 "HTTP-redirect fetch" for `request` ({url, urlList, method, body, headerList, mode, tainting,
   origin}) and the 3xx `redirect`: answers {refusal} for its network errors, or the next request's
   {url, method, body, headerList, referrerPolicy}, where `referrerPolicy` is Referrer Policy §8.2's, from the
   3xx's `Referrer-Policy`, or "" for none. The location URL is §2.2.6's over the one `Location` value, whose
   fragment, where it states none, is the current URL's. */
var _REQUEST_BODY_HEADER_NAMES = ["content-encoding", "content-language", "content-location", "content-type"];
function _followRedirect(redirect, request) {
  var value = redirect.headers.location, loc;
  try { loc = new URL(value, request.url.href); }
  catch (e) { RETHROW_FATAL(e); return { refusal: _refused("network", "redirect-location-failure", [request.urlList[0]], {}) }; }
  if (value.indexOf("#") < 0) loc.hash = request.url.hash;
  if (loc.protocol !== "https:" && loc.protocol !== "http:")
    return { refusal: _refused("network", "blocked-scheme:" + loc.protocol, [request.urlList[0]], {}) };
  if (request.urlList.length - 1 === 20)
    return { refusal: _refused("network", "redirect-count:20", [request.urlList[0]], {}) };
  var credentialsInUrl = loc.username !== "" || loc.password !== "";
  if (credentialsInUrl && ((request.mode === "cors" && request.origin !== loc.origin) || request.tainting === "cors"))
    return { refusal: _refused("network", "blocked-url-credentials", [request.urlList[0]], {}) };
  var method = request.method, body = request.body, headerList = request.headerList;
  if (((redirect.status === 301 || redirect.status === 302) && method === "POST") ||
      (redirect.status === 303 && method !== "GET" && method !== "HEAD")) {
    method = "GET";
    body = null;
    if (headerList) headerList = headerList.filter(function (h) {
      return _REQUEST_BODY_HEADER_NAMES.indexOf(h[0].toLowerCase()) < 0;
    });
  }
  /* "the moment another origin is seen after the initial request, the `Authorization` header is removed" */
  if (headerList && request.url.origin !== loc.origin)
    headerList = headerList.filter(function (h) { return h[0].toLowerCase() !== "authorization"; });
  return { url: loc, method: method, body: body, headerList: headerList,
           referrerPolicy: referrerPolicyFromHeader(redirect.headers["referrer-policy"]) };
}
/* The first and last URL of a request's URL list, the part Fetch §2.2.6 lets script see. */
function _exposedUrlList(urlList) {
  return urlList.length > 1 ? [urlList[0], urlList[urlList.length - 1]] : [urlList[0]];
}

/* Fetch §5.3 "Body mixin"'s body, read chunk by chunk. With no sink this is consume body run to completion
   and the assembled bytes are identical; with `opts.onChunk` each chunk is also released as it arrives.
   Where this runs relative to the gates decides what a released chunk has passed. Before it: the scheme
   allowlist, the userinfo refusal, every hop's private-host, destructive-path and firing checks, and the
   credentialed SOP/CORS check. After it: CORB, the one gate that reads the body.
   A script-like destination is therefore not streamable: `_sniff` classifies most JSON only from the whole
   body (a prefix of a document longer than 4096 bytes does not parse), so a first-chunk answer would be
   CORB's allow arm. The entry refuses `blocked-stream-body-gated:` instead. `computedType` is derived from
   the body, so a head delivered before the body cannot carry one.
   Chunks may come from another realm (`engine/trusted.mjs` runs this file in a vm context with the outer
   `fetch`), so they are never `instanceof`-tested; `%TypedArray%.prototype.set` reads internal slots and
   accumulates them whatever their realm.
   `onRead` is told the byte length of each read the wire answered, so a host can count the body onto its reply
   schedule (lib/reply-schedule.js); a length carries no bytes, so it passes no gate and a body-gated request
   may state it. */
async function _readBody(resp, sink, gated, onRead) {
  /* The entry refuses a body-gated request that asked for chunks, so this cannot fire through it; it is here
     so a later route that skips that refusal aborts at the release point instead of streaming. */
  DCHECK(!(sink !== undefined && gated),
         "a body-gated request reached the chunk release point — CORB is the one gate in this file that reads " +
         "the BODY, a first-chunk sniff answers it the OPPOSITE way for any JSON document over 4096 bytes, and " +
         "a chunk handed over here has not passed it. The entry refuses this with " +
         "`blocked-stream-body-gated:` before the request is made; reaching this line means that refusal was " +
         "removed and the reply is about to be streamed past the only gate that needed all of it");
  DCHECK(sink === undefined || typeof sink === "function",
         "safeFetch was asked for incremental delivery with an `onChunk` that is not callable — the sink is a " +
         "value this zone composes at its own call site, never one the untrusted engine states, so a " +
         "non-function is this zone's contract broken and would throw mid-body with the request already spent");
  DCHECK(onRead === undefined || typeof onRead === "function",
         "safeFetch was given an `onBodyRead` that is not callable — the host composes it at its own call site");
  /* No body is not an empty body. Fetch §5.3 "Body mixin"'s body getter returns null where there is none,
     which a §2.2.3 "Statuses" null body status (101, 103, 204, 205, 304) produces. The answer is the empty
     byte sequence, as `arrayBuffer()` gives, and a sink is called zero times; a sink that must tell "no body"
     from "not yet" reads the returned record. */
  if (resp.body === null) return _NO_BYTES();
  var reader = resp.body.getReader();
  var parts = [], total = 0, step, chunk, i, out, off;
  /* No bound: no chunk cap, size cap or read count. A cap truncates a reply, and a truncated reply is a wrong
     answer about the server rather than a smaller one; the floor is the platform's memory. A body the server
     keeps sending keeps this call pending for as long as the server talks; one it holds silent ends at the
     wire's idle end (WIRE_IDLE_TIMEOUT_MS), which rejects the read. A sink gives a consumer the bytes in time;
     it does not give it the head, since `computedType` needs the whole body. */
  for (;;) {
    step = await reader.read();
    if (step.done) break;
    chunk = step.value;
    /* The bytes are a stranger's and are never asserted on; that the reader yields a byte view is this host's
       contract. A non-view would corrupt silently (`set` reads a string as zeroes). No input reaches this,
       since a server states bytes and never their carrier. `ArrayBuffer.isView` for the cross-realm reason
       above. */
    DCHECK(ArrayBuffer.isView(chunk),
           "the response body reader yielded a chunk that is not a byte view — the bytes themselves are a " +
           "stranger's and are never asserted on, but the SHAPE of a chunk is this host's contract, and a " +
           "non-view accumulates as zeroes into the body that every gate below and the engine read as the reply");
    /* Released before it is accumulated, because timeliness is what a sink is for. Accumulation continues
       either way, so every gate below reads the same record whether or not a sink was stated. */
    if (sink !== undefined) sink(chunk);
    if (onRead !== undefined) onRead(chunk.byteLength);
    parts.push(chunk);
    total += chunk.byteLength;
  }
  out = new Uint8Array(total);
  off = 0;
  for (i = 0; i < parts.length; i++) { out.set(parts[i], off); off += parts[i].byteLength; }
  return out;
}
/* @security-contract  ENFORCEMENT POINT (the single network chokepoint)
   Guarantees: a non-GET only where a default arm admitted it, and never credentialed; http(s) only; no userinfo; cookies omitted unless `opts.credentialed`, and then the
   reply passes SOP/CORS (same-origin with `opts.pageOrigin`, else exact-origin ACAO and ACAC `true`); the
   origin-relative SSRF rule; the destructive-path deny list on credentialed non-`observed` requests; the
   egress policy's firing walk; CORB for script-like destinations; for a page's request, Fetch §4.1 "Main
   fetch"' response tainting (`cors` passes §4.10 "CORS check" and is CORS-filtered, cross-origin `no-cors` is
   opaque, cross-origin `same-origin` is a network error), stated as the record's `type`. Every gate before
   the wire judges each URL of a redirect chain before it is sent (a hop the HTTP cache answers, Fetch §4.6,
   is sent by no one); every gate on the reply, the landed URL. Principals, per call: the asking document's
   record (`opts.doc`) carries the IP address space the SSRF target is classified against; `opts.pageOrigin`
   (`MessageSender.origin` or the host-assigned cluster origin, never parsed from a URL or taken from the
   engine) is the SOP principal and every page request's origin; an opaque one is same-origin with nothing.
   Credentialed callers are `bridge.js`'s `navigationLoad` and `frontierRederive`, where
   `navigationCarriesSession` holds; `fetched` and `fetchedXhr` are not. Every caller states `destination`,
   `mode`, `provenance`, `pinned`, `doc`, `actor`, `credentialed` and `credentials`, asserted at the entry. */
async function safeFetch(url, opts) {
  /* The fired-send record: one entry per URL of this request the zone judged or answered, in order, as
     {url, vector, parts, admittedBy, how, refusal}. `vector` is every signal's value (`_signalVector`), `parts`
     the class of each part the `bytes` row folds (`_bytesParts`), `admittedBy`
     the firing decision's, and `how` says whether the wire was reached: `sent` (a send whose reply later failed
     included), `answered-locally` (the document's record of a byte-identical send, or the HTTP cache, whose
     entry has a null vector because a hit is no act to judge), `not-sent`. It rides every reply record,
     refusals included; a request refused before the firing question has no entry. */
  var egress = [], r;
  try {
    r = await _safeFetchJudged(url, opts, egress);
    r.egress = egress;
    return r;
  } finally {
    egress.forEach(function (e) {
      var k = e.how + " " + (e.vector === null ? "cache" : e.admittedBy === null ? "none" : e.admittedBy.kind);
      _EGRESS_CENSUS[k] = (_EGRESS_CENSUS[k] || 0) + 1;
    });
  }
}
async function _safeFetchJudged(url, opts, egress) {
  opts = opts || {};
  /* The option set this file reads is closed, so a caller's statement it will not read is refused at the
     entry rather than dropped after the bytes are back. */
  _refuseUnreadOptions(opts);
  /* Every fact the firing walk decides from is read once, here, and passed down. The request is judged
     twice (before the wire and after a redirect), and a fact re-read at each gate is one the two can
     disagree about. Reading them at the entry also makes each CHECK fire on every path, not only on paths
     that reach the firing question. */
  var destination = _destinationOf(opts);
  var provenance = _provenanceOf(opts);
  var pinnedMark = _pinnedOf(opts);
  var doc = _docOf(opts);
  var actor = _actorOf(opts);
  var driven = _drivenOf(opts);
  var address = _addressOf(opts);
  /* Fetch §2.2.5 "Requests"' method and body, as §4.5 "HTTP-redirect fetch" changes them. */
  var _method = _methodOf(opts), _body = _bodyOf(opts), _bodyClass = _bodyClassOf(opts);
  /* Derived once, above every gate it scopes; `_credentialedOf` also refuses a header list on a
     cookie-bearing request. */
  var credentialed = _credentialedOf(opts);
  /* RFC 9110 §9.2.1 "Safe Methods": the person's session is spent only on a safe request. The page's own
     non-GET leaves uncredentialed. */
  CHECK(!credentialed || _method === "GET",
        "safeFetch was asked for a credentialed " + _method + " — a request carrying the person's cookies is " +
        "a GET, and a caller composing another verb states `credentialed: false`");
  /* A credentialed request speaks as a document: `_speakAs` sets that document's Cookie and the zone's fetch
     omits credentials. One with no identity would go out with this extension's own jar, whose partition is
     the extension's, and its reply's Set-Cookie would be stored there and in the person's jar. */
  CHECK(!credentialed || (!!opts.identity && typeof opts.identity === "object"),
        "safeFetch was asked for a credentialed request with no identity to speak as — it would send this " +
        "extension's jar and store the reply's cookies where no page of the person's set them");
  var mode = _modeOf(opts);
  /* The request's origin for everything the analysed page's own code asks for: the browser-stated
     `MessageSender.origin` of the document (or the agent-cluster origin this zone assigned an instance), passed
     by the host from its own record and never taken from the engine. Fetch §4.1 "Main fetch" compares the
     request's current URL with it to choose the response tainting. `""` is a principal nobody recorded and is
     treated as an opaque origin, same-origin with nothing. A request this tool composed has this extension's
     origin, whose `<all_urls>` host permission is the browser's answer to the CORS question, so no page origin
     is asked of it. */
  CHECK(actor !== "page" || mode === "navigate" || typeof opts.pageOrigin === "string",
        "safeFetch was asked for a request the analysed page made without the page's origin — Fetch §4.1 " +
        "\"Main fetch\" decides from it whether the page may read the reply (§4.10 \"CORS check\" against it, " +
        "or an opaque response), so a page request with none would be read as same-origin with whatever it " +
        "named. State the origin this zone holds for the requesting document");
  CHECK(actor !== "page" || mode !== "cors" || _CREDENTIALS_MODES.indexOf(opts.credentials) >= 0,
        "safeFetch was asked for a `cors` request the analysed page made without its credentials mode — " +
        "Fetch §4.10 \"CORS check\" reads the request's credentials mode to decide whether `*` passes, so the " +
        "check would be decided by silence");
  CHECK(actor !== "page" || mode !== "cors" || typeof opts.useCorsPreflight === "boolean",
        "safeFetch was asked for a `cors` request the analysed page made without its use-CORS-preflight flag " +
        "(Fetch §2.2.5 \"Requests\") — §4.1 \"Main fetch\" preflights a request with the flag set whatever " +
        "its method and headers, so an unstated flag would decide by silence whether the server is asked first");
  CHECK(opts.headers === undefined || _headerListIs(opts.headers),
        "safeFetch was handed a header list that is not `[name, value, class]` entries — the CORS-unsafe " +
        "request-header names are computed over it and each value's class is judged, and a list this zone " +
        "cannot read would be sent unjudged. The class is the engine's word for the value's bytes, or " +
        "`concrete` for a list this zone composed");
  var _pageOrigin = typeof opts.pageOrigin === "string" ? opts.pageOrigin : "";
  var parsed;
  try { parsed = new URL(String(url)); }
  /* Network: a string the URL Standard §6.1 "URL class" constructor refuses never becomes a request in a
     browser either. There is no URL, so the URL list is empty and the engine's `response.url` is "". */
  catch (e) { return _refused("network", "bad-url", [], {}); }
  /* Network: Fetch §4.3 "Scheme fetch" ends its switch with "Return a network error", so a page fetching
     `file:` gets §5.6's TypeError in a browser too. This allowlist keeps a crafted URL off local and extension
     resources. */
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:")
    return _refused("network", "blocked-scheme:" + parsed.protocol, [parsed.href], {});
  /* Network: URL Standard §4.2 "URL miscellaneous" — "A URL includes credentials if its username or password
     is not the empty string" — and Fetch §5.4 "Request class" throws a TypeError for one, which `fetch()`
     rejects with (§5.6 "Fetch methods"). A browser refuses this request identically and no widening reopens
     it. Without this line the `fetch` below would throw and the address would leave no record at all.
     It is a refusal and not an egress signal: a signal is something a person may permit, and this cannot be
     sent at any setting. A redirect to a userinfo URL is refused by the browser itself (Fetch §4.5
     "HTTP-redirect fetch": cors mode, credentials in locationURL, cross-origin), so it is not re-asked.
     The reason names no ground because the ground is the secret; the URL list carries the href the caller
     already holds. */
  if (parsed.username !== "" || parsed.password !== "")
    return _refused("network", "blocked-url-credentials", [parsed.href], {});
  /* Decline: a script-like destination cannot be streamed (see `_bodyGated`), and no browser refuses a
     streamed script, so only this tool declines. Serving it whole to a caller that asked for chunks would
     leave that caller believing it streamed past the one gate that needs the whole body. */
  if (opts.onChunk !== undefined && _bodyGated(opts))
    return _refused("decline", "blocked-stream-body-gated:" + _destinationOf(opts), [parsed.href], {});
  /* Origin-relative SSRF, Local Network Access §2.2 "Local Network Request": a request whose target's IP
     address space is less public than "request's policy container's IP address space". That space is the
     asking document's record's (docRecordAddressSpace), which the zone set when it provisioned the document:
     its response's for a document created from a network response, its creator's for one whose container is
     a clone (Local Network Access §3.5 "Integration with HTML"). It is per call, because the trusted zone
     interleaves many renderers, and never parsed from an address the request states: a cross-origin script
     acts in the document it is loaded into, and a `data:` child of a loopback page reaches loopback. */
  var _pageSpace = docRecordAddressSpace(doc);
  /* The person's stated table is awaited, not assumed (see `_EXPLORED_STATING`); waiting at the chokepoint
     covers every door this zone has. Once the table is stated the test is false and costs no microtask, and
     `_EXPLORED_STATED` never returns to false. */
  if (!_EXPLORED_STATED && _EXPLORED_STATING !== null) await _EXPLORED_STATING;
  /* Two records: `opts.identity` is who the request speaks as (its client, Fetch §2.2.5 "Requests"), and
     `pageOrigin` the document that reads the reply. They are one document for every request but a
     navigation's, whose client is the document that started it while its reader is the one it creates. */
  CHECK(!opts.identity || mode === "navigate" ||
        opts.identity.origin === (_isRealOrigin(_pageOrigin) ? _pageOrigin : "null"),
        "a " + mode + " request speaks as " + (opts.identity && opts.identity.origin) + " while its reply is read " +
        "by " + _pageOrigin + " — only a navigation's client differs from its reader, and the preflight and " +
        "the reply would be CORS-checked as two different requesters");
  /* The analysed page's own read, whose response tainting Fetch §4.1 "Main fetch" decides; a navigation is
     judged by the instance that becomes its reader, and a request this tool composed is read by this
     extension, whose `<all_urls>` host permission is the browser's answer to the CORS question. */
  var _pageRead = actor === "page" && mode !== "navigate";
  /* Fetch §2.2.5 "Requests"' URL list, current URL, response tainting, header list and speaking identity, as
     §4.5 "HTTP-redirect fetch" changes them. Each pass is one §4.1 "Main fetch": every gate runs on the URL
     before it is sent, a redirect is followed by this zone and never by the platform (`_redirectOf`), and a
     hop is sent as its own request, speaking as the identity under its own rule. */
  /* The header list as the wire and the gates below read it, `[name, value]` pairs. Each pair's class is kept
     by the pair's identity, which a redirect's filter preserves (`_followRedirect`). */
  var _headerClass = new Map();
  var _headerList = opts.headers === undefined ? undefined : opts.headers.map(function (h) {
    var pair = [h[0], h[1]];
    _headerClass.set(pair, h[2]);
    return pair;
  });
  var _urlList = [parsed.href], _current = parsed, _tainting = "basic";
  var _identity = opts.identity, resp;
  /* Every hop's credentialed Set-Cookie, in order, for the asking flow's jar (`_speakAs`). */
  var _setCookieList = [];
  for (;;) {
    var _hop = _urlList.length > 1;
    /* The sites this pass asks about (the redirect taint, the speak-as Sec-Fetch-Site, the hop's identity),
       in one round trip: the URL it sends, the page that reads the reply and the identity it speaks as. */
    await siteAsk(siteHostsOf([_current.href, _isRealOrigin(_pageOrigin) ? _pageOrigin : "null",
                               _identity ? _identity.origin : "null"]));
    /* Fetch §4.1 "Main fetch"' switch: `basic` while the current URL is the page's origin and nothing has
       tainted the request; then a network error in `same-origin` mode, `opaque` in `no-cors`, `cors`
       otherwise. Tainting never returns to `basic`. */
    if (_pageRead && !(_tainting === "basic" && _isRealOrigin(_pageOrigin) && _current.origin === _pageOrigin)) {
      if (mode === "same-origin")
        return _refused("network", "blocked-mode-same-origin:" + _current.origin, [parsed.href], {});
      _tainting = mode === "no-cors" ? "opaque" : "cors";
    }
    /* Network: host permissions bypass the browser's own answer, and this reinstates it for the addresses
       this zone can classify, before the request or the hop is sent. A refusal carries the requested URL
       alone: Fetch §2.2.6 gives a network error the empty URL list, its consumers read the request's URL off
       this one, and a private address a redirect named, which a network error never exposes, stays out. */
    var _targetSpace = _ipAddressSpace(_current.hostname);
    if (_lessPublicThan(_targetSpace, _pageSpace))
      return _refused("network", "blocked-local-network" + (_hop ? "-redirect:" : ":") + _targetSpace + "-from-" +
                      _pageSpace, [parsed.href], {});
    /* Fetch §4.1 "Main fetch": a `cors`-tainted request whose use-CORS-preflight flag is set, or whose method
       is not CORS-safelisted, or whose header list has CORS-unsafe names, runs §4.4 "HTTP fetch" with
       makeCORSPreflight, and with no CORS-preflight cache §4.4 always runs §4.8 "CORS-preflight fetch" first.
       Its failure is a network error, and the request itself is never sent. Its origin, and the origin the
       replies are CORS-checked against, is Fetch §2.2.5's serialized request origin: "null" once the URL
       list's redirect-taint is not `same-origin`. */
    var _unsafeNames = _tainting === "cors" ? _corsUnsafeRequestHeaderNames(_headerList || []) : [];
    var _preflight = _tainting === "cors" &&
                     (opts.useCorsPreflight || !_corsSafelistedMethod(_method) || _unsafeNames.length > 0);
    var _serializedOrigin = _isRealOrigin(_pageOrigin) && requestRedirectTaint(_pageOrigin, _urlList) === "same-origin"
      ? _pageOrigin : "null";
    CHECK(!_preflight || (_identity && typeof _identity === "object" && typeof _identity.origin === "string"),
          "safeFetch must CORS-preflight this request and was handed no request identity — Fetch §4.8's " +
          "preflight carries the request's origin and is CORS-checked against it, and that origin is the " +
          "trusted zone's `identityFor(request)`, which the caller passes as `opts.identity`");
    /* Credentials ride where Fetch §4.4 "HTTP fetch" includes them: mode `include`, or `same-origin` while the
       tainting is `basic`. */
    var _cookies = credentialed && (!_identity || _identity.credentials === "include" || _tainting === "basic");
    /* Fetch §4.6 "HTTP-network-or-cache fetch", asked before the firing decision: a hit sends nothing, so it
       is no act to judge. The partition is the identity's (lib/http-cache.js's httpCachePartitionOf), read
       once what earlier sessions stored is restored (httpCacheStated). The Vary fields are the header list
       and the speak-as set this send would carry, composed only for a URL the cache holds, so a request it
       cannot answer reads no cookie before its firing decision. A request that needs a CORS preflight sends
       it even on a fresh hit, since §4.4 runs §4.8 before §4.6, so its hit is judged as a send and answered
       in place of the wire once the preflight passes (`_answered`). */
    var _restoring = httpCacheRestoring();
    if (_restoring) await _restoring;
    /* WebSockets §2.2 "Opening handshake" step 2: the handshake's cache mode is "no-store". */
    var _partition = httpCachePartitionOf(_identity), _cache = mode === "websocket" ? null : httpCacheFor(_partition);
    var _sendFacts = _identity ? { credentials: _identity.credentials, partition: _partition } : null;
    var _cacheFields = null, _cacheMode = "no-store", _stored = null, _sent;
    if (_cache !== null) {
      _cacheFields = httpCacheRequestFields(_headerList, {});
      _cacheMode = httpCacheMode("default", _cacheFields);
      if (_cacheMode !== "no-store" && httpCacheHolds(_cache, _current.href))
        _stored = httpCacheSelect(_cache, _current.href, _method, httpCacheRequestFields(_headerList,
          (await _speakAsRequest(_identity, _urlList, _method, mode, destination, _cookies)).headers), Date.now());
    }
    /* A hit, or a 304 to its validation, is a reply read from the entry, so an entry of another partition would
       hand this document the answer to another cookie jar's request. */
    CHECK(_stored === null ||
          (_stored.entry.partition.topLevelSite === _partition.topLevelSite &&
           _stored.entry.partition.hasCrossSiteAncestor === _partition.hasCrossSiteAncestor &&
           _stored.entry.partition.opaque === _partition.opaque),
          "the HTTP cache chose for " + _current.href + " in the partition " +
          JSON.stringify(_partition) + " an entry stored for " +
          JSON.stringify(_stored && _stored.entry.partition) + " — Fetch §2.8 gives each network partition key " +
          "its own cache");
    var _answered = _stored !== null && _stored.fresh;
    if (_answered && !_preflight) {
      /* The record carries no grade: the host attaches the asking flow's provenance where it delivers the
         reply, so a forced arm answered here stays forced and is never merged into observed data. */
      _sent = { resp: httpCacheResponse(_stored), setCookies: [] };
      egress.push({ url: _current.href, vector: null, parts: null, admittedBy: null, how: "answered-locally",
                    refusal: null });
    } else {
      /* The firing decision, before this URL is requested (see `_firingRefusal`). Decline: no browser
         refuses this; this tool declines to spend the act. The flow parks rather than running its failure
         path, so it fires the day the origin is widened, and the address is still reported. A widening is a
         statement about one host, so a hop to another is asked again, and `url-authority` reads the hop's
         own address. */
      var _signalFacts = { url: _current, destination: destination, provenance: provenance,
                           pinned: pinnedMark, doc: doc, actor: actor, driven: driven, address: address,
                           bodyClass: _body === null ? "none" : _bodyClass,
                           method: _method, credentialed: credentialed,
                           headers: _headerList && _headerList.map(function (h) {
                             return [h[0], h[1], _headerClass.get(h)]; }) };
      var _decision = _firingDecision(_signalFacts);
      var _entry = { url: _current.href, vector: _decision.vector, parts: _bytesParts(_signalFacts),
                     admittedBy: _decision.admittedBy, how: "not-sent", refusal: _decision.refusal };
      egress.push(_entry);
      var _ptok = _decision.refusal;
      var _declined = _ptok === null ? null
        : _refused("decline", "blocked-signal" + (_hop ? "-redirect:" : ":") + _ptok, _exposedUrlList(_urlList), {});
      /* A refused request byte-identical to a send this document's unforced run made is served from that
         send's record (lib/send-coalesce.js) and reaches no other party: `send` below asks the record, by the
         key the wire would carry, in place of the wire. The key holds the speak-as set, so the request is
         composed as a send is (which reads the identity's cookies), and only where that run sent this method
         to this URL at all; its reply's cookies stay out of the person's jar.
         Named residual. Not covered: a request that needs a CORS preflight, whose OPTIONS is a send of its
         own, and a request that speaks as no document (engine/trusted.mjs's navigation load). Next diff: ask
         the record for the preflight's key too. Its absence shows as a forced world's preflighted request,
         identical to one the unforced run sent, recorded `not-sent`. */
      var _keyStart = _wireSendKeyStart(_method, _current.href);
      var _served = _ptok !== null && !!_identity && !_preflight &&
                    sendCoalesceSome(_identity.client.docKey, function (key, madeBy) {
                      return madeBy === "default" && key.slice(0, _keyStart.length) === _keyStart;
                    });
      if (_ptok && !_served) return _declined;
      var _reached = function (made) {
        CHECK(!(made && _served),
              "a refused request was about to reach the wire as one served from the record — the record is " +
              "asked for its key before the wire is");
        if (made) _entry.how = "sent";
        else if (_entry.how === "not-sent") _entry.how = "answered-locally";
      };
      /* The destructive-path deny list, before this URL is requested (see `_destructiveToken`). It is scoped
         to credentialed requests, because without cookies a logout path ends no session, and to provenance
         other than `observed`. An `observed` credentialed request is the ambient seed: the person's own
         browser just made this exact request in this profile, so refusing to repeat it prevents nothing.
         Decline, and permanent: no widening reopens this list. A browser would send this request; this tool
         will not send it with the person's session, and a hop is sent with it, so a hop is asked too. */
      if (credentialed && provenance !== "observed") {
        var _dtok = _destructiveToken(_current);
        if (_dtok) {
          _entry.refusal = "blocked-destructive:" + _dtok;
          return _refused("decline", "blocked-destructive" + (_hop ? "-redirect:" : ":") + _dtok,
                          _exposedUrlList(_urlList), {});
        }
      }
      /* A WebSocket handshake every gate above admitted. The transport is not this chokepoint's: it holds
         no connection a frame could later be written to, so nothing is sent and the request parks. Decline:
         a browser would connect. */
      if (mode === "websocket") {
        _entry.refusal = "blocked-transport:websocket";
        return _refused("decline", "blocked-transport:websocket", _exposedUrlList(_urlList), {});
      }
      var init = { method: _method, credentials: "omit", redirect: "manual" };
      if (_body !== null) init.body = _body;
      /* The caller's header list. On the XHR path it is the analysed bundle's own list, which is within the
         model for the request this file issues (an uncredentialed GET whose forbidden header names the
         browser strips) and is refused on a cookie-bearing one by `_credentialedOf`. This file never adds
         auth headers; cookies, in credentialed mode, are the identity's (`_speakAs`) and are gated by the
         SOP/CORS check. The cache mode's fields (httpCacheModeHeaders) and a stale entry's validators
         (httpCacheValidators) are appended after the preflight decision, as §4.6 appends them to its own copy
         of the request. */
      var _sentList = _cache === null ? _headerList
        : (_headerList || []).concat(httpCacheModeHeaders(_cacheMode, _cacheFields),
                                     _stored === null || _answered ? [] : httpCacheValidators(_stored.entry));
      if (_sentList && _sentList.length) init.headers = _sentList;
      if (opts.signal) init.signal = opts.signal;
      /* Fetch §4.1 "Main fetch": the preflight (§4.8) and then the request, as one send. A request with an
         identity speaks as it (`_speakAs`); a preflight failure is a network error and the request is never
         sent. */
      _sent = await _speakAs(_identity, _urlList, _method, mode, destination, _cookies,
                             _decision.admittedBy !== null && _decision.admittedBy.kind === "default",
                             _preflight ? _unsafeNames : null, null, init, async function (speak) {
        /* A wire that rejects is Fetch §5.6's network error ("reject p with a TypeError"), answered as this
           chokepoint's network refusal like every other one; an aborted send rejects to its asker, which
           withdrew it. */
        try { return await send(speak); }
        catch (e) {
          if (_wireIdleErrors.has(e) && !(init.signal && init.signal.aborted))
            return { refusal: _refused("decline", "wire-idle", [parsed.href], {}) };
          if (!_wireNetworkErrors.has(e) || (init.signal && init.signal.aborted)) throw e;
          return { refusal: _refused("network", "network-error:" + e.message +
                                     (e.cause === undefined ? "" : ":" + String(e.cause)), [parsed.href], {}) };
        }
      });
      async function send(speak) {
        if (_preflight) {
          var _pf = await _corsPreflight(_current.href, _method, _headerList || [], opts.credentials,
                                         opts.useCorsPreflight, _serializedOrigin, opts.signal, speak.preflight,
                                         function (i) {
            return _wireSend(_current.href, i, _sendFacts, _reached, _entry.admittedBy.kind).then(function (w) {
              return w.resp;
            });
          });
          if (_pf) return { refusal: _refused("network", "blocked-cors-preflight:" + _pf + ":" + _current.origin,
                                              [parsed.href], {}) };
        }
        if (_answered) return { resp: httpCacheResponse(_stored) };
        init.speakAs = speak === null ? null : speak.request;
        if (_served) {
          /* Only a send a default arm admitted is the unforced run's; no await separates this from the ask. */
          if (sendCoalesceMadeBy(speak.request.docKey, _wireSendKey(_current.href, init, _sendFacts)) !== "default")
            return { refusal: _declined };
          _entry.admittedBy = { kind: "record", refused: _ptok };
          _entry.refusal = null;
        }
        return await _wireSend(_current.href, init, _sendFacts, _reached, _entry.admittedBy.kind);
      }
      if (_sent.refusal) return _sent.refusal;
      if (_cache !== null && !_answered) {
        /* A 304 to a validation is answered from the freshened entry (§4.6: "Set response to storedResponse");
           the 304 itself has a null body (Fetch §2.2.3 "Statuses"), so nothing is left unread. */
        var _validated = httpCacheForwarded(_cache, _cacheMode, _current.href, _method,
                                            httpCacheRequestFields(init.headers, _sent.requestHeaders), _sent.resp,
                                            _sent.requestTime, _sent.responseTime, _stored);
        if (_validated !== null) _sent.resp = _validated;
      }
    }
    resp = _sent.resp;
    DCHECK(Array.isArray(_sent.setCookies), "a send answered no Set-Cookie list — _speakAs states one for every reply");
    _setCookieList = _setCookieList.concat(_sent.setCookies);
    var _redirect = await _redirectOf(resp);
    if (_redirect === null) break;
    /* Fetch §4.4 "HTTP fetch": a `cors`-tainted request's every response, a 3xx included, passes §4.10 "CORS
       check" or is a network error. */
    if (_pageRead && _tainting === "cors") {
      var _rcors = _corsCheck(_redirect.headers, _serializedOrigin, opts.credentials);
      if (_rcors)
        return _refused("network", "blocked-cors:" + _rcors + ":" + _current.origin, [parsed.href], {});
    }
    var _next = _followRedirect(_redirect, { url: _current, urlList: _urlList, method: _method, body: _body,
                                             headerList: _headerList, mode: mode, tainting: _tainting,
                                             origin: _pageOrigin });
    if (_next.refusal) return _next.refusal;
    _method = _next.method;
    _body = _next.body;
    _urlList.push(_next.url.href);
    _current = _next.url;
    _headerList = _next.headerList;
    if (_identity)
      _identity = await identityForRedirect(_identity, _current.href,
                                      _next.referrerPolicy === "" ? _identity.referrerPolicy : _next.referrerPolicy);
  }
  /* The URL the bytes came from: Fetch §2.2.5 "Requests": "A request has an associated current URL. It is a
     pointer to the last URL in request's URL list", and §4.1 "Main fetch" bases readability on its origin, not
     on the address requested. */
  var _finalUrl = _current;
  /* An `https` request landing on `http` is mixed content, which is not this zone's question. The engine asks
     Mixed Content §4.4 "Should fetching request be blocked as mixed content?" before the wire; the
     response-side §4.5 "Should response to request be blocked as mixed content?" is a named residual at
     core/fetch/mixed_content.h. A scheme test here would be wrong both ways: §4.5 depends on §4.3 "Does
     settings prohibit mixed security contexts?" over the document's settings and ancestors, which this zone
     does not hold; Secure Contexts §3.1 "Is origin potentially trustworthy?" treats loopback as trustworthy;
     and a top-level navigation is exempt. What this zone owes that residual is `urlList`, whose last item is
     the response URL §4.5 judges. */
  var _finalOrigin = _finalUrl.origin;
  var _exposed = _exposedUrlList(_urlList);
  /* Is the landed resource same-origin with the page principal: one answer, read by both CORB's exemption and
     the credentialed SOP, so the two cannot disagree. `pageOrigin` is the browser-stated
     `MessageSender.origin` and is never parsed from a URL: a sandboxed frame has an ordinary address and an
     opaque origin, and parsing the address would fabricate a tuple origin with same-origin access to its
     embedder. An opaque or absent principal is same-origin with nothing, so it gets strict CORB and a
     credentialed read that needs CORS, which it can never be granted (ACAO cannot equal it). */
  var _resourceSameOrigin = _isRealOrigin(_pageOrigin) && _isRealOrigin(_finalOrigin) &&
                            _finalOrigin === _pageOrigin;
  /* The headers, unwrapped. Fetch §5.5 "Response class"' headers getter returns this's headers, whose
     iteration throws only what the callback throws; an invented empty map would have CORB judge an unlabelled
     body and the SOP read an absent `Access-Control-Allow-Origin`. */
  var headers = {};
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
  /* Own SOP/CORS for a credentialed reply. The browser does not apply the same-origin policy to an extension
     fetch with host permissions, so when cookies are attached this zone enforces it on the bytes before
     returning them; otherwise a bundle could record a cross-origin endpoint and read the person's
     authenticated data from any site they are signed into.
       SOP:  a resource same-origin with the page principal (`opts.pageOrigin`) is readable.
       CORS: a cross-origin credentialed read needs `Access-Control-Allow-Origin` equal to that exact origin
             (never `*`) and `Access-Control-Allow-Credentials: true`.
     A refused read returns no body; the request was a GET, so what is refused is the bytes. */
  if (credentialed) {
    /* `_pageOrigin` and `_resourceSameOrigin` are read, not recomputed: they are over `_finalOrigin`, so a
       same-origin request that redirected cross-origin is cross-origin here and at CORB alike. An opaque or
       absent principal is same-origin with nothing and can never match ACAO, so it fails closed. */
    if (!_resourceSameOrigin) {
      var _acao = headers["access-control-allow-origin"] || "";
      var _acac = (headers["access-control-allow-credentials"] || "").toLowerCase();
      // Fetch §4.10 "CORS check": ACAO must byte-match the request's own origin (`*` is
      // refused once credentials mode is "include") and ACAC must be `true`.
      if (!_isRealOrigin(_pageOrigin) || _acao !== _pageOrigin || _acac !== "true")
        /* Network: Fetch §4.10 "CORS check" failing makes §4.4 "HTTP fetch" "return a network error". The
           reason names the landed origin that failed. Headers are `{}` here, unlike CORB's refusal: this
           refuses a cross-origin read the server never granted, and its headers are part of that read. */
        return _refused("network", "blocked-cors-credentialed:" + _finalOrigin, _exposed, {});
    }
  }
  /* Fetch §4.1 "Main fetch"' response tainting, for a request the analysed page made, as the walk above left
     it: `basic`; `cors`, whose final reply passes §4.10 "CORS check" against the serialized request origin
     and is §2.2.6's CORS filtered response; or `opaque`. A `fetch()` (destination "") is the one consumer that
     reads an opaque response itself, so its body is not read and no bytes reach the engine; an element (a
     classic script, an image, a stylesheet) consumes the internal response, so its bytes are delivered beside
     the `opaque` type and the engine keeps them from page script. */
  var _type = _tainting;
  if (_tainting === "cors") {
    var _cors = _corsCheck(headers, _serializedOrigin, opts.credentials);
    if (_cors)
      return _refused("network", "blocked-cors:" + _cors + ":" + _finalOrigin, _exposed, {});
    headers = _corsFilteredHeaders(headers, opts.credentials);
  } else if (_tainting === "opaque" && destination === "") {
    /* The body is given back unread: nothing may read an opaque response's body. */
    if (resp.body) {
      try { await resp.body.cancel(); } catch (e) { RETHROW_FATAL(e); }
    }
    return { ok: resp.ok, status: resp.status, statusText: "", headers: {}, body: _NO_BYTES(),
             urlList: _exposed, computedType: "", refusal: null, type: _type, setCookies: _setCookieList };
  }
  /* The body, as bytes, after every hop's SSRF check, so nothing internal is ingested before the target is judged.
     There is no decode: the engine decodes with the response's charset and BOM (see the header), so it
     receives exactly what the server sent. It is read after the credentialed SOP/CORS check above, which needs
     only headers (Fetch §4.4 "HTTP fetch" runs the CORS check on the response before its body is delivered),
     and before CORB, the one gate that reads the body (see `_readBody`). */
  /* A body the wire failed after its head is Fetch §5.6's network error to every reader of this record, which
     is answered whole; one the wire went silent on is this zone's `wire-idle` decline (WIRE_IDLE_TIMEOUT_MS).
     An abort is the caller's, so it is the caller's to read. */
  /* The head, once every gate that reads only headers has run and before the body is read: Fetch §5.6 "Fetch
     methods" resolves a `fetch()` here, and the body that follows is read below. It carries no computed type,
     which is derived from the whole body (`_sniff`), and it is refused to a body-gated request, whose reply
     CORB judges on the body. */
  if (opts.onHead !== undefined) {
    DCHECK(typeof opts.onHead === "function" && !_bodyGated(opts),
           "safeFetch was given an `onHead` that is not callable, or for a body-gated request whose head may " +
           "not be released before CORB has read its body — the host composes it at its own call site");
    opts.onHead({ status: resp.status, statusText: resp.statusText, headers: headers, urlList: _exposed,
                  type: _type });
  }
  var body;
  try { body = await _readBody(resp, opts.onChunk, _bodyGated(opts), opts.onBodyRead); }
  catch (e) {
    RETHROW_FATAL(e);
    if (opts.signal && opts.signal.aborted) throw e;
    if (_wireIdleErrors.has(e)) return _refused("decline", "wire-idle", _exposed, headers);
    if (!_wireNetworkErrors.has(e)) throw e;
    return _refused("network", "network-error:" + e.message + (e.cause === undefined ? "" : ":" + String(e.cause)),
                    _exposed, {});
  }
  /* The one sniff and the one type: CORB reads `protected` and the record reads `type`, so no second answer
     exists to disagree. A missing `Content-Type` is MIME Sniffing §5.1 "Interpreting the resource metadata"'s
     "the supplied MIME type is undefined", and is passed as absent. */
  var _nosniff = _determineNosniff(headers["x-content-type-options"]);
  var _sn = _sniff(body);
  var _computed = _computedType(headers["content-type"], _nosniff, _sn);
  /* CORB by the request's destination (Fetch §2.2.5). A script-like destination is bytes that will run as
     code under QuickJS, so a cross-origin body must be JavaScript-typed; every other destination ("" for a
     `fetch()`, "image", "font", "style" …) is data and exempt. Whether the bytes later reach QuickJS is the
     caller's contract; this returns bytes.
     The gate applies to every reply, whatever the status. Fetch §2.10 "Should response to request be blocked
     due to its MIME type?" reads the header list, the destination and the essence, never a status, and an
     authenticated error page is exactly the cross-origin data CORB keeps out of the renderer's process. The
     engine's separate refusal of a non-ok script (HTML §8.1.4.2 "Fetching scripts", `script_fetch_status_ok`)
     keeps a refused response from becoming a program; neither rule is the other's guard. */
  if (_isScriptLike(_destinationOf(opts))) {
    // The declared essence is what CORB's tables are stated over; the sniff confirms a body whose label lied.
    var _declared = String(headers["content-type"] == null ? "" : headers["content-type"])
      .split(";")[0].trim().toLowerCase();
    var _deny = _corbDeniesScript(_declared, _nosniff, _sn, _resourceSameOrigin);
    /* The deciding rule rides the status message with the computed type, as every refusal names its ground.
       Network: Chromium's CORB, and ORB after it, refuse this load too, and an ORB-blocked script load reaches
       the page as Fetch §5.6's network error; this zone applies the rule only because a host-permission fetch
       bypasses the browser's copy.
       Named residual. Not covered: `same-origin-protected` refuses a page its own HTML or JSON as script
       text, which a browser hands over and then fails at compile. Next diff: return same-origin bytes and let
       the engine's compile fail them. Its absence shows as a same-origin script load ending in an `error`
       event where Chrome reports a SyntaxError. */
    if (_deny)
      return _refused("network", "blocked-corb:" + _deny + ":" + _computed,
                      _exposed, headers);
  }
  /* The type this zone computed travels with the bytes, so the renderer is handed an answer rather than
     evidence (`core/fetch/fetch.c` asserts its presence on the reply record). `refusal: null` is the positive
     statement that the request reached the wire and this is the reply, including an HTTP error status.
     Status 0 is what every refusal answers and no HTTP response has; `mode` is unset, so Fetch's default
     `cors` applies and no opaque filtered response can answer 0, which is what the DCHECK below binds. */
  DCHECK(resp.status !== 0,
         "the wire answered with status 0, which is the one status no HTTP response has and which every " +
         "refusal in this file answers with — `mode` is unset so Fetch's default `cors` applies and an " +
         "opaque filtered response cannot arise, so a 0 here is a reply and a refusal wearing one number");
  return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers: headers, body: body,
           urlList: _exposed, computedType: _computed,
           refusal: null, type: _type, setCookies: _setCookieList };
}

/* The IP address space of the policy container a document at `url` is created with, HTML §7.1.7 "Policy
   containers"' determine navigation params policy container with the field Local Network Access §3.5
   "Integration with HTML" adds: a local URL (about:, blob:, data:) with an initiator takes a clone of the
   initiator's container, so `creator`'s record's space; an http(s) response's container takes the space of
   the response's IP address, which this zone answers from the host (`_ipAddressSpace`; see SECURITY.md's
   private network by name); anything else is a new container, initially public. `creator` is the initiating
   document's record, or null for a document no record of this zone initiated. */
function safeFetchDocumentAddressSpace(url, creator) {
  CHECK(creator === null || docRecordIs(creator),
        "a document's address space was asked with a creator that is neither null nor a record docRecord minted");
  var parsed = new URL(String(url));
  var local = parsed.protocol === "about:" || parsed.protocol === "blob:" || parsed.protocol === "data:";
  if (local && creator !== null) return docRecordAddressSpace(creator);
  if (parsed.protocol === "http:" || parsed.protocol === "https:") return _ipAddressSpace(parsed.hostname);
  return "public";
}

/* Fetch §4.3 "Scheme fetch"'s "data" arm, for a document load: "Let dataURLStruct be the result of running the
   data: URL processor on request's current URL" (Fetch §6 "data: URLs"), a network error on failure, and
   otherwise a response whose status message is `OK`, whose header list is the struct's serialized MIME type as
   `Content-Type`, and whose body is the struct's body. It is not safeFetch: a data: URL's bytes are in the URL
   and the load reaches no other party, so none of safeFetch's signals has a value to compute. The processor is
   the wire's platform fetch (`_wire`), which runs §4.3 for a data: URL without a network. The
   record is safeFetch's shape, and its type is computed by the same sniff. */
async function safeFetchSchemeData(url) {
  var parsed = new URL(String(url));
  var resp;
  CHECK(parsed.protocol === "data:",
        "safeFetchSchemeData was asked to load " + JSON.stringify(parsed.href) + " — only §4.3's \"data\" arm " +
        "reaches no other party, so every other scheme goes through safeFetch");
  try { resp = await _wire(parsed.href, { method: "GET", credentials: "omit", redirect: "manual" }); }
  catch (e) {
    /* Network: §4.3 answers a data: URL the processor refuses with a network error, which the platform's
       fetch reports as a TypeError. */
    RETHROW_FATAL(e);
    return _refused("network", "data-url-failure", [parsed.href], {});
  }
  var headers = {};
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
  DCHECK(resp.status === 200 && resp.statusText === "OK" && typeof headers["content-type"] === "string",
         "the realm's fetch answered a data: URL with status " + resp.status + " " +
         JSON.stringify(resp.statusText) + " and no Content-Type — §4.3's \"data\" arm returns a response whose " +
         "status message is `OK` and whose header list is the struct's MIME type, so this realm's fetch is " +
         "not the processor this function binds to");
  var body = new Uint8Array(await resp.arrayBuffer());
  var computed = _computedType(headers["content-type"], _determineNosniff(headers["x-content-type-options"]),
                               _sniff(body));
  return { ok: true, status: resp.status, statusText: resp.statusText, headers: headers, body: body,
           urlList: [parsed.href], computedType: computed, refusal: null, type: "basic", setCookies: [] };
}

/* Fetch §2.2.2 "Headers": a forbidden request-header is one "forbidden so the user agent remains in full
   control over them". A script's value for one is ignored, so a request this zone sends refuses it by name
   rather than letting the person believe it was sent. Returns the refused name, or null. */
var _FORBIDDEN_REQUEST_HEADER_NAMES = [
  "accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method",
  "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin",
  "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via",
];
var _METHOD_OVERRIDE_HEADER_NAMES = ["x-http-method", "x-http-method-override", "x-method-override"];
function fetchForbiddenRequestHeader(pairs) {
  for (var i = 0; i < pairs.length; i++) {
    var lower = pairs[i][0].toLowerCase();
    if (_FORBIDDEN_REQUEST_HEADER_NAMES.indexOf(lower) >= 0 || lower.startsWith("proxy-") || lower.startsWith("sec-"))
      return pairs[i][0];
    // "getting, decoding, and splitting" the value, then §2.2.1 "Methods"' forbidden methods.
    if (_METHOD_OVERRIDE_HEADER_NAMES.indexOf(lower) >= 0 && pairs[i][1].split(",").some(function (m) {
          return ["CONNECT", "TRACE", "TRACK"].indexOf(m.trim().toUpperCase()) >= 0; }))
      return pairs[i][0];
  }
  return null;
}

/* The Send panel's stated record, {origin, referer, jar}, each null for the frame's own:
   - `origin` {header}: the Origin sent, a serialized origin or "null", or null for none at all;
   - `referer`: the Referer sent;
   - `jar` {tabId, documentId}: the open frame whose cookie partition the send speaks with, picked from
     lib/send.js's operatorJarDomain and resolved there through chrome.cookies.getPartitionKey, so no
     partition key is ever typed. */
function safeFetchStatedShape(stated) {
  return !!stated && typeof stated === "object" && Object.keys(stated).length === 3 &&
         (stated.origin === null || (!!stated.origin && Object.keys(stated.origin).length === 1 &&
           (stated.origin.header === null || typeof stated.origin.header === "string"))) &&
         (stated.referer === null || typeof stated.referer === "string") &&
         (stated.jar === null || (!!stated.jar && Number.isInteger(stated.jar.tabId) &&
           typeof stated.jar.documentId === "string" && stated.jar.documentId !== ""));
}

/* The person's typing is input: a stated value that is not what its header carries is refused, never sent.
   Answers the refusal reason, or null. The operator tier and lib/send.js's identity preview both ask it. A
   jar is never typed: it names a frame, and lib/send.js refuses one whose frame is gone. */
function safeFetchStatedRefusal(stated) {
  var o = stated.origin === null ? null : stated.origin.header;
  if (o !== null && o !== "null" &&
      !(URL.canParse(o) && /^https?:$/.test(new URL(o).protocol) && new URL(o).origin === o))
    return "stated-origin-not-an-origin:" + o;
  if (stated.referer !== null &&
      !(URL.canParse(stated.referer) && /^https?:$/.test(new URL(stated.referer).protocol) &&
        new URL(stated.referer).href === stated.referer && !stated.referer.includes("#")))
    return "stated-referer-not-a-url:" + stated.referer;
  return null;
}

/* The operator tier: a request the person composed in the popup, sent as the identity of a frame they
   picked (`opts.identity`, identityFor's answer over lib/request-identity.js's frameIdentity). Any method and
   a body. No firing decision and no destructive deny list: a person authorized these bytes at a surface that
   showed them, and a token list there would veto their act. What a browser refuses still applies: the
   scheme, credentials in the URL, a forbidden request-header, a same-origin-mode request to another origin,
   and Fetch §4.8's preflight. The reply is returned whole, whether or not Fetch §4.10's CORS check would let
   the frame's own script read it.
   `opts.stated` is the Send panel's record (safeFetchStatedShape), for testing a server's CORS or CSRF
   checks; its jar is already in `opts.identity`. This is the one tier that passes an Origin or Referer to the
   speak-as rule; the preflight is then CORS-checked against the stated Origin, which is what the server was
   sent, or the identity's when the person stated that none is sent. `Sec-Fetch-*` stays the frame's. */
async function safeFetchOperator(url, opts) {
  CHECK(typeof senderIsExtensionDocument === "function" && senderIsExtensionDocument(opts.sender),
        "an operator send arrived from " + JSON.stringify(opts.sender && opts.sender.origin) + " — only an " +
        "extension document (the router's own predicate) carries a person's act, and this tier spends the " +
        "person's cookies with no deny list");
  CHECK(!!opts.identity && typeof opts.identity.origin === "string",
        "an operator send names no identity — it speaks as a frame the person picked, never as this extension");
  CHECK(typeof opts.method === "string" && opts.method !== "" && Array.isArray(opts.headers) &&
        (opts.body === null || typeof opts.body === "string" || opts.body instanceof Uint8Array) &&
        ["cors", "no-cors", "same-origin"].indexOf(opts.mode) >= 0,
        "an operator send is missing its method, header pairs, body statement or mode");
  CHECK(safeFetchStatedShape(opts.stated),
        "an operator send states no {origin, referer, jar} record — lib/send.js passes the Send panel's, or " +
        "PAGE_CONTEXT_STATED_NONE for a probe");
  var stated = opts.stated;
  var statedRefusal = safeFetchStatedRefusal(stated);
  if (statedRefusal !== null) return _refused("network", statedRefusal, [], {});
  var parsed;
  try { parsed = new URL(String(url)); } catch (e) { return _refused("network", "bad-url", [], {}); }
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:")
    return _refused("network", "blocked-scheme:" + parsed.protocol, [parsed.href], {});
  if (parsed.username !== "" || parsed.password !== "")
    return _refused("network", "blocked-url-credentials", [parsed.href], {});
  var forbidden = fetchForbiddenRequestHeader(opts.headers);
  if (forbidden !== null)
    return _refused("network", "forbidden-request-header:" + forbidden, [parsed.href], {});
  /* The send and each hop of it, as safeFetch walks them (Fetch §4.5 "HTTP-redirect fetch"), with the
     operator tier's gates: what a browser refuses, and no policy of this tool's. A hop speaks as the same
     identity, under its own rule, with the Origin and Referer the person stated. */
  var urlList = [parsed.href], current = parsed, tainting = "basic", method = opts.method, body = opts.body;
  var headerList = opts.headers, identity = opts.identity, resp;
  for (;;) {
    /* The sites this pass asks about, as safeFetch's pass asks them, and the Origin the person stated. */
    await siteAsk(siteHostsOf([current.href, identity.origin,
                               stated.origin !== null && stated.origin.header !== null ? stated.origin.header : "null"]));
    var sameOrigin = current.origin === identity.origin;
    if (!(tainting === "basic" && sameOrigin)) {
      if (opts.mode === "same-origin")
        return _refused("network", "blocked-mode-same-origin:" + current.origin, [parsed.href], {});
      tainting = opts.mode === "no-cors" ? "opaque" : "cors";
    }
    var unsafe = _corsUnsafeRequestHeaderNames(headerList);
    var preflight = tainting === "cors" && (!_corsSafelistedMethod(method) || unsafe.length > 0);
    var init = { method: method, headers: headerList, redirect: "manual" };
    if (body !== null) init.body = body;
    var checkedOrigin = stated.origin !== null && stated.origin.header !== null ? stated.origin.header
      : requestRedirectTaint(identity.origin, urlList) === "same-origin" ? identity.origin : "null";
    /* The person's own act, speaking as a frame they picked: its reply's cookies are theirs to keep. */
    var sent = await _speakAs(identity, urlList, method, opts.mode, "", true, true, preflight ? unsafe : null, stated,
                              init, async function (speak) {
      /* A wire that rejects is graded as safeFetch grades it, since who reads the answer changes nothing a
         browser does: a silent wire is this zone's `wire-idle` decline and a failed one Fetch §5.6's network
         error. Anything else is not the wire's answer and travels on. */
      try {
        if (preflight) {
          /* The person's act is sent, never answered from an earlier send's record, so its preflight takes the
             wire its request takes. */
          var pf = await _corsPreflight(current.href, method, headerList, identity.credentials, false,
                                        checkedOrigin, undefined, speak.preflight,
                                        function (i) { return _wire(current.href, i); });
          if (pf) return { refusal: _refused("network", "blocked-cors-preflight:" + pf + ":" + current.origin,
                                             [parsed.href], {}) };
        }
        init.speakAs = speak.request;
        return { resp: await _wire(current.href, init) };
      } catch (e) {
        RETHROW_FATAL(e);
        if (_wireIdleErrors.has(e)) return { refusal: _refused("decline", "wire-idle", [parsed.href], {}) };
        if (!_wireNetworkErrors.has(e)) throw e;
        return { refusal: _refused("network", "network-error:" + e.message, [parsed.href], {}) };
      }
    });
    if (sent.refusal) return sent.refusal;
    resp = sent.resp;
    /* The person's send is in the "no-store" cache mode: it reads its answer from the server and stores
       nothing. It still invalidates (httpCacheInvalidate), since an unsafe method the server accepted changes
       what the partition's stored responses describe. Its header list is sent as typed, so §4.6's no-store
       fields are not appended to it. A restore still in flight would bring back what this invalidates, so it
       is waited for first. */
    var restoring = httpCacheRestoring();
    if (restoring) await restoring;
    var cache = httpCacheFor(httpCachePartitionOf(identity));
    if (cache !== null) httpCacheInvalidate(cache, method, current.href, resp.status);
    var redirect = await _redirectOf(resp);
    if (redirect === null) break;
    var next = _followRedirect(redirect, { url: current, urlList: urlList, method: method, body: body,
                                           headerList: headerList, mode: opts.mode, tainting: tainting,
                                           origin: identity.origin });
    if (next.refusal) return next.refusal;
    urlList.push(next.url.href);
    current = next.url;
    method = next.method;
    body = next.body;
    headerList = next.headerList;
    identity = await identityForRedirect(identity, current.href,
                                   next.referrerPolicy === "" ? identity.referrerPolicy : next.referrerPolicy);
  }
  var headers = {};
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
  /* A body the wire failed or went silent on is graded as a rejected send is. */
  var bytes;
  try { bytes = new Uint8Array(await resp.arrayBuffer()); }
  catch (e) {
    RETHROW_FATAL(e);
    if (_wireIdleErrors.has(e)) return _refused("decline", "wire-idle", _exposedUrlList(urlList), headers);
    if (!_wireNetworkErrors.has(e)) throw e;
    return _refused("network", "network-error:" + e.message, _exposedUrlList(urlList), headers);
  }
  return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers: headers, body: bytes,
           urlList: _exposedUrlList(urlList), computedType: "", refusal: null };
}
/* The person's configuration check of an attacker site (lib/attacker-site.js): one GET of a fresh name under the
   site, with no cookies, no Origin or Referer a document would send, and no redirect followed, answered with the
   requirements it fails. It is the person's act at the popup's setting, so no firing decision applies; its one
   destination is the site they typed, parsed before anything is sent. The body is not read. */
async function safeFetchAttackerSiteCheck(site) {
  CHECK(attackerSiteRefusal(site) === null,
        "the attacker site check was asked of " + JSON.stringify(site) + ", which the setting's own parse refuses");
  var resp;
  try {
    resp = await _wire(attackerSiteProbeUrl(site), { method: "GET", headers: [], credentials: "omit",
                                                     redirect: "manual" });
  } catch (e) {
    RETHROW_FATAL(e);
    DCHECK("netError" in e, "the wire rejected the attacker site check without stating the browser's reason or " +
                            "its absence — wireSend states one on every failure");
    return attackerSiteRequirements({ error: String((e && e.message) || e), netError: e.netError });
  }
  var headers = {};
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
  if (resp.body) await resp.body.cancel();
  return attackerSiteRequirements({ status: resp.status, headers: headers });
}
/* The chokepoint and the policy that decides whether it fires, installed together so no host holds half the
   contract. `engine/trusted.mjs` runs this file in a vm context and reads them off it; `ast-worker.html`
   loads it into the offscreen document before `bridge.js`.
   Writers: `safeFetchPermit` (one signal, one value, one origin), `safeFetchWiden` and `safeFetchUnwiden` (a
   whole origin), `safeFetchEgressStated` (the host's one restore, answering the entries it dropped) and
   `safeFetchEgressStating` (the restore's read, which every request awaits). `safeFetchEgressTable` is what a
   host persists, in the shape the restore takes. For a surface: `safeFetchWidenable` and
   `safeFetchSignalUsable` (the tests `safeFetchPermit` aborts on, so a surface can refuse with the reason),
   `safeFetchSignals`, `safeFetchPermitted`, `safeFetchSignalVector`, `safeFetchDefaultArms` and
   `safeFetchWidenedOrigins`. `safeFetchFiringRefusal` answers for an act that is not a fetch, in the reply
   record's refusal vocabulary. Every one resolves to `_firingRefusal`,
   `_signalVector` or a predicate they compare against, so none is a second policy.
   There is no exported destructive-path check: the deny list is a floor under requests this file composes
   and fires on its own, and never overrules an operator's explicit act (`safeFetchOperator`). */
if (typeof self !== "undefined") {
  self.safeFetch = safeFetch;
  self.safeFetchAttackerSiteCheck = safeFetchAttackerSiteCheck;
  self.safeFetchOperator = safeFetchOperator;
  self.safeFetchSchemeData = safeFetchSchemeData;
  self.safeFetchDocumentAddressSpace = safeFetchDocumentAddressSpace;
  self.safeFetchStatedRefusal = safeFetchStatedRefusal;
  self.safeFetchStatedShape = safeFetchStatedShape;
  self.fetchForbiddenRequestHeader = fetchForbiddenRequestHeader;
  self.safeFetchHeaderListIs = safeFetchHeaderListIs;
  self.safeFetchWiden = safeFetchWiden;
  self.safeFetchEgressUnnamed = safeFetchEgressUnnamed;
  self.safeFetchUnwiden = safeFetchUnwiden;
  self.safeFetchPermit = safeFetchPermit;
  self.safeFetchEgressStated = safeFetchEgressStated;
  self.safeFetchEgressStating = safeFetchEgressStating;
  self.safeFetchEgressTable = safeFetchEgressTable;
  self.safeFetchWidenable = safeFetchWidenable;
  self.safeFetchSignalUsable = safeFetchSignalUsable;
  self.safeFetchSignals = safeFetchSignals;
  self.safeFetchPermitted = safeFetchPermitted;
  self.safeFetchSignalVector = safeFetchSignalVector;
  self.safeFetchDefaultArms = safeFetchDefaultArms;
  self.safeFetchFiringRefusal = safeFetchFiringRefusal;
  self.safeFetchCrossDocumentRefusal = safeFetchCrossDocumentRefusal;
  self.safeFetchWidenedOrigins = safeFetchWidenedOrigins;
  self.safeFetchSiteSetting = safeFetchSiteSetting;
  self.safeFetchSetSiteSetting = safeFetchSetSiteSetting;
  self.safeFetchEgressReady = safeFetchEgressReady;
  self.safeFetchEgressCensus = safeFetchEgressCensus;
}
