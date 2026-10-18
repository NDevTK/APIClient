/* THE ABORT DOCUMENT: DOM §3.2's AbortSignal, its timeout and any(), and Fetch §5.4's dependent signal.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ABORT_H
#define ENGINE_HOST_FIXTURE_ABORT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ABORT[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var ac0c = new AbortController(); var ac0 = ac0c.signal;"
    "ac0.addEventListener({ toString: function(){ var n = 0; for (var i = 0; i < 500; i++) { n += i; } return 'abort'; } }, function(){ fetch('/api/idlcoerce?v=coerced'); });"   /* addEventListener's `type` is a Web IDL DOMString: the toString is the PAGE's code and has a loop in it, so the machine parks on step_tostring_run and resumes at the exact stage — and the listener is registered under the string that call RETURNED, which the abort below proves by firing it */
    "ac0c.abort();"
    "var ac = new AbortController(); ac.signal.addEventListener('abort', function(e){ fetch('/api/aborted?v=' + (e.isTrusted && e.type === 'abort' && e.target === ac.signal ? 'fired' : 'wrong')); }); ac.abort();"   /* the controller's signal is the REAL state machine: abort() reads [[Signal]] as an internal slot and fires `abort`, whose listener runs as its own task on this flow */
    "var tsig = AbortSignal.timeout({ valueOf: function(){ var n = 0; for (var i = 0; i < 2000; i++) { n += i; } return n; } });"   /* [EnforceRange] unsigned long long is ToNumber on the PAGE's object: the loop inside valueOf preempts, so the timeout machine must suspend and resume at the exact stage it parked on */
    "if (tsig.aborted) { fetch('/api/deadline?v=expired'); } else { fetch('/api/deadline?v=live'); }"   /* a timeout's aborted flag is UNKNOWN, so both arms run and the fallback path's endpoint is learned too */   /* THE responsive gate: a bundle routes, hosts assets and often bases its API on this, so both arms must be reached */   /* the desktop-vs-touch gate, the same shape over a numeric member */
    /* DOM §3.2's "create a dependent abort signal" STEP 2, which IS `AbortSignal.any(signals)`'s method steps,
       asked from inside the step machine (solver/engine.c's `engine_prepare_fork`). The controller is FRESH:
       the two above are concretely aborted, so step 2 would return at whichever element held one.
       `anyfork` IS THE ASK: `tsig.aborted` is unknown at element 0, so step 2 forks there — the TRUE arm is born
       aborted with the timeout's reason, the FALSE arm yields a live dependent signal — and both endpoints are
       learned.
       `anylate` IS THE CURSOR, and real pages take this shape (`[<a controller's signal>,
       AbortSignal.timeout(<n>)]`; core/dom/abort.h carries the corpus derivation): the concrete flag first, so the fork
       happens at element ONE. A resume that restarted at element 0 would be asked about `anyac.signal`'s flag
       while the seam held `tsig`'s.
       `anyiter` parks the same machine twice: a page iterator with a loop in `next` suspends it at Web IDL
       §3.2.21.1 step 3, and step 2 then forks it, so the held flag and the iterator cursor are separate slots
       that each survive one clone. `Symbol.iterator` is assigned rather than written as a computed key. */
    "var anyac = new AbortController();"
    "var anya = AbortSignal.any([tsig, anyac.signal]);"
    "if (anya.aborted) { fetch('/api/anyfork?v=anyhot'); } else { fetch('/api/anyfork?v=anycold'); }"
    "var anyb = AbortSignal.any([anyac.signal, tsig]);"
    "if (anyb.aborted) { fetch('/api/anylate?v=latehot'); } else { fetch('/api/anylate?v=latecold'); }"
    "var anyit = {}; anyit[Symbol.iterator] = function(){ var _k = 0;"
    "   return { next: function(){ var n = 0; for (var i = 0; i < 500; i++) { n += i; }"
    "     return _k++ === 0 ? { done: false, value: tsig } : { done: true, value: undefined }; } }; };"
    "var anyc = AbortSignal.any(anyit);"
    "if (anyc.aborted) { fetch('/api/anyiter?v=iterhot'); } else { fetch('/api/anyiter?v=itercold'); }"
    "</script>"
    /* FETCH §5.4 "Request class" STEP 30 — "Set this's signal to the result of creating a dependent abort
       signal from signals, using AbortSignal and this's relevant realm" — asked from the CONSTRUCTOR.
       THE CLAIM IS THE ENDPOINT PAIR. DOM §3.2's step 2 is "For each signal of signals: if signal is aborted,
       then set resultSignal's abort reason to signal's abort reason and return resultSignal", and `tsig`'s
       flag is unknown, so step 30 forks inside the constructor and `reqsig.signal.aborted` is concrete and
       opposite on the two arms: both `/api/reqsigfork` endpoints are learned.
       `anyfork` above is not a control for this: it proves `AbortSignal.any`'s machine carries the sibling and
       says nothing about §5.4's constructor.
       The payloads are literals, so a reached arm cannot read as an aborted one. `tsig` is declared in this
       document's first script, which is why the statement belongs here. */
    "<script>"
    "var reqsig = new Request('/api/reqsig', { signal: tsig });"
    "if (reqsig.signal.aborted) { fetch('/api/reqsigfork?v=reqhot'); } else { fetch('/api/reqsigfork?v=reqcold'); }"
    "</script>"
    /* A request whose signal the page aborts after its response arrived, as Apollo does once it has read its
       own copy. Fetch §5.6 "Fetch methods"' abort steps error the response's body only while it is readable,
       so a body the page finished reading stays read and one it had not started rejects with the signal's
       reason; Chrome 155 answers the same. The request is the page's either way, so both stay recorded. Each
       read is reported by whether it settled: the reply's bytes are unknown external input, so a test of their
       length would fork. `clone` is a clone taken at the answer, read after the abort. Fetch §2.2.4 "Bodies"'
       clone a body tees the stream and keeps out1 for the response; §5.6's abort errors only the response's
       own body, and Streams §4.9.1's ReadableByteStreamTee forwards to both branches only the source's
       error, so the clone reads every byte. Chrome 155 rejects it with an AbortError instead; the row follows
       the spec. */
    "<script>"
    "(async function () {"
    " var c = new AbortController();"
    " var r = await fetch('/api/apread', { method: 'POST', body: 'x', signal: c.signal });"
    " var k = r.clone(), own; try { await r.text(); own = 'read'; } catch (e) { own = e.name; } c.abort();"
    " var kv; try { await k.text(); kv = 'read'; } catch (e) { kv = e.name; }"
    " var c2 = new AbortController();"
    " var r2 = await fetch('/api/apunread', { method: 'POST', body: 'x', signal: c2.signal });"
    " c2.abort(); var late; try { await r2.text(); late = 'read'; } catch (e) { late = e.name; }"
    " fetch('/api/apabort?own=' + own + '&late=' + late + '&clone=' + kv);"
    "})();"
    "</script>"
    /* The same tee with the response unread at the abort: out1 is readable, so the abort errors it and the
       response's read rejects with the reason, while the clone's branch is untouched and reads. */
    "<script>"
    "(async function () {"
    " var c4 = new AbortController();"
    " var r4 = await fetch('/api/apteed', { method: 'POST', body: 'x', signal: c4.signal });"
    " var k4 = r4.clone(); c4.abort();"
    " var o4; try { await r4.text(); o4 = 'read'; } catch (e) { o4 = e.name; }"
    " var q4; try { await k4.text(); q4 = 'read'; } catch (e) { q4 = e.name; }"
    " fetch('/api/apteedab?own=' + o4 + '&clone=' + q4);"
    "})();"
    "</script>"
    /* Fetch §4.1 "Main fetch" queues processResponse when the response arrives, so a zero-delay timer queued
       after fetch() runs before the response settles the promise. */
    "<script>"
    "(function () {"
    " var seen = [];"
    " fetch('/api/xffetch').then(function () { seen.push('reply'); fetch('/api/xforder?v=' + seen.join('-')); });"
    " setTimeout(function () { seen.push('timer'); }, 0);"
    "})();"
    "</script>"
    /* XHR §3.5.6's asynchronous send() fetches in parallel, so its flow runs on: a zero-delay timer queued
       after send() runs before the send's `load`. §3.5.7 abort() and §3.5.1 open() reach the fetch controller
       the send set, on the line after it: abort() answers with its request error steps, open() ends the send
       with no event, and neither reply is delivered. */
    "<script>"
    "(function () {"
    " var x = new XMLHttpRequest(), seen = []; x.open('GET', '/api/xatimer');"
    " x.onload = function () { seen.push('load'); fetch('/api/xaorder?v=' + seen.join('-')); };"
    " x.send(); setTimeout(function () { seen.push('timer'); }, 0);"
    " var y = new XMLHttpRequest(), ev = []; y.open('GET', '/api/xaabort');"
    " ['abort', 'load', 'error', 'loadend'].forEach(function (t) {"
    "  y.addEventListener(t, function () { ev.push(t); }); });"
    " y.send(); y.abort();"
    " var z = new XMLHttpRequest(), zv = []; z.open('GET', '/api/xaopen');"
    " ['abort', 'load', 'error', 'loadend'].forEach(function (t) {"
    "  z.addEventListener(t, function () { zv.push(t); }); });"
    " z.send(); z.open('GET', '/api/xaopen2');"
    " setTimeout(function () {"
    "  fetch('/api/xaaborted?v=' + ev.join('-') + '&rs=' + y.readyState);"
    "  fetch('/api/xareopened?v=' + (zv.join('-') || 'none') + '&rs=' + z.readyState);"
    " }, 0);"
    "})();"
    "</script>"
    /* A request aborted while its reply is still owed: the abort runs on the line after the call, before the
       host can answer, so the engine tells the host to end the send, and the page sees an AbortError. */
    "<script>"
    "(async function () {"
    " var c3 = new AbortController(), p3 = fetch('/api/apcancel', { signal: c3.signal }); c3.abort();"
    " var v; try { await p3; v = 'resolved'; } catch (e) { v = e.name; }"
    " fetch('/api/apcancelled?v=' + v);"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
