/* THE FETCH DOCUMENT: Fetch requests, their replies, bodies and headers, consumed by the page.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FETCHREPLY_H
#define ENGINE_HOST_FIXTURE_FETCHREPLY_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FETCHREPLY[] =
    "<!doctype html><html>"
    "<body>"
    "<script>var cfg = { admin: state.admin };</script>"
    "<script>"
    "(async function(){ var c = await (await fetch('/api/config')).json(); fetch('/api/user?region=' + c.region); })();"   /* FETCH-AWAIT-RESULT: await a safe GET, then §6.4.3 json() over the host's bytes — the parsed body's field flows into a later endpoint as a concrete example */
    /* THE SAME REPLY THROUGH THE OTHER SPELLING, AND THE ROW IS THE SECOND REACTION. Every statement above
       reaches the reply with `await`, which is §27.5.4.7.1 PromiseResolve ( ctor, resolution ) inside Await;
       this is `.then(r => r.json()).then(…)`, which is how essentially every shipped bundle reads a config and
       is a DIFFERENT arrangement of the same two settlements — the reaction's derived promise is resolved with
       json()'s promise (an ordinary Promise), and json()'s own promise is separately resolved with the parsed
       record. Only the second of those can go wrong, and when it does the FIRST reaction still runs: the tell
       is a run in which `chain1` is present and `chain2` is absent, with no page error, nothing parked and a
       frontier reporting that it drained. Both tags ride ONE address so that the two are one comparison rather
       than two rows that could each be missing for their own reason, and the region rides with them because a
       second reaction that runs without its value is a third outcome again. */
    "fetch('/api/config').then(function(r){ fetch('/api/chain?at=chain1');"
      " return r.json(); }).then(function(c){ fetch('/api/chain?at=chain2-' + c.region); });"
    /* §6.4 clone(), which is how a caching or interceptor layer is written: copy the reply, read the copy, and
       still hand the original on. Both halves of the spec are asserted because both are the reason it exists —
       the clone reads INDEPENDENTLY (its own body-used latch, or the copy would be dead the moment the original
       was read), and cloning does NOT consume the original (or the layer would have nothing to pass along).
       The throw is SYNCHRONOUS on an already-read body, not a rejected promise, so a page's try/catch sees it
       where it was written. */
    "(async function(){ var r = await fetch('/api/config'); var c = r.clone();"
      " var a = await c.json(); var b = await r.json();"
      " var thrown = 'no'; try { r.clone(); } catch (e) { thrown = 'sync'; }"
      " fetch('/api/clonebody?copy=' + a.region + '&orig=' + b.region + '&used=' + thrown); })();"
    /* §6.4.1 arrayBuffer() / §6.4.2 bytes(): the reply as the BYTE SEQUENCE it is. They read the same reply two
       ways and report the SAME length and the SAME first byte ('{'), which is what says the two are one body and
       not two decodings of it — and a length that survives says the record carries one rather than recovering it
       with a strlen. Two clones, because reading is single-use. */
    "(async function(){ var r = await fetch('/api/config');"
      " var ab = await r.clone().arrayBuffer(); var by = await r.clone().bytes();"
      " fetch('/api/bodybytes?len=' + ab.byteLength + '&n=' + by.length + '&b0=' + by[0]"
      "   + '&kind=' + (ab instanceof ArrayBuffer) + (by instanceof Uint8Array)); })();"
    /* THE BODY-USED LATCH TIME-TRAVELS. The Response is awaited BEFORE the concolic branch, so ONE reply object
       is shared by both arms — and each then reads it. The latch is C state in the class opaque, which no
       property hook can see, so without a COW capture the first arm to read left the reply consumed for the
       other and the sibling's own first read threw. Both bodyADMIN and bodyPUBLIC present ⇒ each arm saw its own
       unread reply, which is the same isolation every shared JS slot already had. */
    "(async function(){ var r = await fetch('/api/config');"
      " var tag = cfg.admin ? 'bodyADMIN' : 'bodyPUBLIC';"   /* THE FORK — before either arm has read `r` */
      " var v = (await r.json()).region + '-' + tag;"        /* both arms read the SAME reply */
      " fetch('/api/bodyiso?v=' + v); })();"
    /* A BYTE BODY WHOSE BYTES CAME FROM AN UNKNOWN — the one shape ECMAScript §10.4.5.18
       TypedArraySetElement's span store exists for. `screen.width` is the source (a real one, unlike the
       synthetic `num`).
       THE UNKNOWN IS AT INDEX 1 BETWEEN CONCRETE NEIGHBOURS: a span over the whole body would be satisfied by a
       store that recorded one span per write with no offset arithmetic, so bracketing it makes the offset the
       claim.
       POST, WHICH IS NEVER FIRED: RFC 9110 §9.2.1's safe set does not contain it, so its values come only from
       the local forced-exec path and the row asserts about a DERIVED record. */
    "(function(){ var u8 = new Uint8Array(4);"
      " u8[0] = 65; u8[1] = screen.width; u8[2] = 66; u8[3] = 67;"
      " fetch('/api/bodyspan', { method: 'POST', body: u8 }); })();"
    /* TWO METHODS, ONE ADDRESS — the request's IDENTITY, and the one thing no other probe here can see. The
       reply seam listed URLs and matched on URLs, so this page's two requests were ONE line on the join (the
       dedup dropped the POST) and ONE delivery filling BOTH entries: the POST's promise settled with the GET's
       reply and the page read a body the server produced for a request it never made. Every @H value and every
       @S verdict behind such a line came from that. `g=GET&p=POST` is what the pair-keyed seam produces and
       `p=GET` is what the URL-keyed one produced, so the two are told apart by the emitted record itself. */
    "(async function(){ var g = await fetch('/api/echo');"
      " var p = await fetch('/api/echo', { method: 'POST', body: 'x' });"
      " fetch('/api/verb?g=v' + g.headers.get('x-echo-method') + '&p=v' + p.headers.get('x-echo-method')); })();"
    /* A REPLY WHOSE BYTES ARE JAVASCRIPT IS A PROGRAM: "a fetch whose body is JAVASCRIPT is ALWAYS fetched +
       EXECUTED". The population is exactly this shape — a `fetch()` whose body the page neither `eval`s nor
       appends as a `<script>` — so neither reaction touches the body: each reads ONE header and reports it,
       a statement about what ARRIVED.
       THE `.then` IS WHAT REACHES THE ARM: the arm sits at the DELIVERY, and a fire-and-forget `fetch(...)`
       emits its record at the REQUEST whatever becomes of the reply; a reaction keeps the flow parked on it.
       THE MARKERS ARE CONSTANTS: every marker path is a literal, here and in the two served programs, and the
       one derived value (the header readback) rides a query param of a constant path. */
    "(function(){"
      " fetch('/chunk/replyprog.js').then(function(r){"
      "  fetch('/api/progseen?ct=' + r.headers.get('content-type')); });"
      " fetch('/chunk/replyctl.js').then(function(r){"
      "  fetch('/api/ctlseen?ct=' + r.headers.get('content-type')); }); })();"
    /* §5 Headers. The RECORD fill is the conversion `fetch(u, {headers: {...}})` performs, so it is exercised
       through the interface that states it: a record init, then the members that read it back. The list keeps
       PAIRS — two `set-cookie` appends stay two entries and getSetCookie reads both — while `get` combines them
       per §2.2.2 Headers, which is the whole difference between a header list and a map. A name is lowercased on the
       way in, an absent header is null rather than "", and `set` replaces every entry with that name. */
    "(function(){ var h = new Headers({'X-Api-Key': 'k1', 'Accept': 'application/json'});"
      " h.append('Set-Cookie', 'a=1'); h.append('Set-Cookie', 'b=2');"
      " var sc = h.getSetCookie();"
      " h.append('X-Api-Key', 'k2');"                       /* append KEEPS both -> get joins them */
      " var joined = h.get('x-api-key');"                   /* case-insensitive: normalized on the way in */
      " h.set('X-Api-Key', 'k3');"                          /* set REPLACES every entry with that name */
      " fetch('/api/hdrs?acc=' + h.get('Accept') + '&sc=' + sc.length + ':' + sc[0] + ':' + sc[1]"
      "   + '&join=' + joined + '&set=' + h.get('X-Api-Key')"
      "   + '&has=' + h.has('accept') + h.has('nope') + '&miss=' + h.get('nope')); })();"
    /* THE INIT'S ARM IS DECIDED BY @@iterator, which is a READ of the page's object and not a type test. A
       plain bag with a PROXY in front of it still converts as a record — its `ownKeys` and its `get` are the
       page's code, driven as requests — and the proxied reads are what the record arm is made of. If the read
       of @@iterator were not part of the algorithm this would still pass; what it proves is that a trap-bearing
       init survives the conversion with its headers intact. */
    "(function(){ var seen = '';"
      " var p = new Proxy({'X-Trap': 'tv'}, { get: function(t, k){ seen = 'trapped'; return t[k]; } });"
      " var h2 = new Headers(p);"
      " fetch('/api/hdrproxy?v=' + h2.get('x-trap') + '&t=' + seen); })();"
    /* §5.2's iterable<>. Iteration is NOT the raw list: it SORTS by name and COMBINES each name's values, so
       two `x-a` appends are ONE entry — while `set-cookie` stays one entry per value, which is the whole reason
       the list keeps pairs. forEach hands the page (value, key) in that order, and its callback is the page's
       code driven as a request. `for...of` over the Headers itself is `entries`, per §3.7.9 Iterable
       declarations' PAIR-ITERATOR arm, which defines @@iterator and `entries` as the SAME function object.
       Not §3.7.10 Asynchronous iterable declarations: Headers writes `iterable<>`, not `async iterable<>`. */
    "(function(){ var h = new Headers({'x-b': '2'});"
      " h.append('x-a', '1'); h.append('x-a', '9');"
      " h.append('Set-Cookie', 'c1'); h.append('Set-Cookie', 'c2');"
      " var ks = Array.from(h.keys()).join('|');"
      " var vs = Array.from(h.values()).join('|');"
      " var es = ''; for (var e of h) { es += e[0] + '=' + e[1] + ';'; }"
      " var fe = ''; h.forEach(function(v, k, t){ fe += k + ':' + v + ';'; if (t !== h) fe += 'BADTHIS'; });"
      " fetch('/api/hdriter?k=' + ks + '&v=' + vs + '&e=' + es + '&f=' + fe); })();"
    /* WEB IDL §3.7.9.1 "Default iterator objects"' ITERATOR IN A CYCLE WITH THE OBJECT IT ITERATES — the shape
       its class's gc_mark is observable through. The record behind the iterator's opaque owns `target`, which
       the collector can walk only if the class declares a mark; with a free and no walk, `gc_decref` never
       subtracts the internal reference and neither object is collected. `h.it = h.entries()` builds the
       cycle (§3.7.9 "Iterable declarations"' iterator is reachable from the object it holds) and the IIFE
       drops the pair.
       NO ROW CAN STATE THE LEAK: `iter-cycle` is a reachability witness. What names it is this host's
       teardown — `JS_RunGC(rt)` then `JS_FreeRuntime(rt)` — whose non-empty gc_obj_list prints `[gcleak]` and
       `[gcroot]` and aborts on `DCHECK(list_empty(&rt->gc_obj_list))`; the statement exists so that walk is
       asked something.
       THE `c` PARAM IS THE CYCLE ITSELF: if Headers took no expando there would be no cycle, so the
       round-trip is read back and emitted. */
    "(function(){ var h = new Headers({'x-cyc': 'cv'});"
      " var it0 = h.entries(); h.it = it0;"
      " var e = it0.next();"
      " fetch('/api/itercycle?v=' + e.value[0] + '&w=' + e.value[1]"
      "   + '&c=' + (h.it === it0 ? 'cyc' : 'nocyc')"
      "   + '&d=' + (it0.next().done ? 'done' : 'more')); })();"
    /* §5.1's SEQUENCE arm, which Web IDL picks because the init is ITERABLE. It is the iterator protocol twice
       over — once for the pairs, once for each pair's two items — so a GENERATOR init proves it is the protocol
       and not an array walk, and a MAP proves the same for the shape that is iterable without being an array.
       A pair that is not exactly two items is a TypeError, and null is not "no init": HeadersInit is a union of
       object types that Web IDL does not make nullable. */
    "(function(){"
      " function* g(){ yield ['x-gen', 'g1']; yield ['x-gen', 'g2']; }"
      " var a = new Headers(g()).get('x-gen');"
      " var b = new Headers(new Map([['x-map', 'm1']])).get('x-map');"
      " var c = new Headers([['x-arr', 'a1']]).get('x-arr');"
      " var bad = 'no'; try { new Headers([['only-one']]); } catch (e) { bad = 'threw'; }"
      " var nul = 'no'; try { new Headers(null); } catch (e) { nul = 'threw'; }"
      " fetch('/api/hdrseq?g=' + a + '&m=' + b + '&a=' + c + '&bad=' + bad + '&nul=' + nul); })();"
    /* THE RECORD'S DEDUP IS A DIFFERENT EQUIVALENCE THAN THE HEADER LIST'S, and that is the whole statement.
       Web IDL §3.2.23 converts `record<ByteString, ByteString>` with "Let typedKey be key converted to an IDL
       value of type K … Set result[typedKey] to typedValue" — K is ByteString, which does not case-fold, so
       `X-Rec` and `x-rec` are TWO entries of the record and the fill sees both. §5.1's fill then "append(key,
       value) to headers" for each, and Fetch §2.2.2 Headers' get returns "the values of all headers … separated
       from each other by 0x2C 0x20": "r1, r2", over ONE name in the list. A replace loop matching the LOWERCASED name
       stood in the fill and answered "r2" — one pair silently discarded, which for this tool is a header the
       report would not carry. */
    "(function(){ var h = new Headers({'X-Rec': 'r1', 'x-rec': 'r2'});"
      " fetch('/api/hdrrec?v=' + h.get('x-rec') + '&k=' + Array.from(h.keys()).join('|')); })();"
    /* HTML §4.10.22.8's ESCAPE, on the one input that shows it: a field name is written INSIDE QUOTES in the
       part's Content-Disposition, so "the result of the encoding … must be escaped by replacing any 0x0A (LF)
       bytes with the byte sequence `%0A`, 0x0D (CR) with `%0D` and 0x22 (") with `%22`. The user agent must
       not perform any other escapes." Unescaped, `a"\r\nb` closes the quoted name and opens a second header
       line — the part boundary forged from inside the body — so BOTH halves are asserted: the escaped spelling
       is present AND the raw `a"` is absent, because a serializer that wrote the name twice would satisfy the
       first alone. The name already carries a CRLF pair, which is exactly what step 1's newline normalization
       leaves alone, so this statement is about the escape and nothing else. */
    "(function(){ var fd = new FormData(); fd.append('a\"\\r\\nb', 'v');"
      " new Response(fd).text().then(function(t){"
      "   fetch('/api/mpesc?esc=' + (t.indexOf('name=\"a%22%0D%0Ab\"') >= 0)"
      "     + '&raw=' + (t.indexOf('a\"') >= 0)); }); })();"
    /* THE TRANSPORT REQUIREMENT REACHES THE SURFACE. `init.headers` is read and converted, and the endpoint
       carries what the request needs — which is the half of "usable" the @H surface never had. The
       Authorization value is built out of `state`, so it is a CONCOLIC and reports its SHAPE: the `{hole}` is
       what tells a reviewer this one is a runtime value to supply, while the two literal headers are the real
       strings the code computed. Inventing a token for the first would be a fabricated observation. */
    "fetch('/api/needsauth', { method: 'POST', headers: {"
      " 'Authorization': 'Bearer ' + state.token,"
      " 'X-Api-Version': '2024-11-01',"
      " 'Content-Type': 'application/json' } });"
    /* THE OTHER TWO PLACES A REQUEST CARRIES A VALUE — the PATH and the BODY. The surface named only the query
       string, so a templated address was one opaque row and a POST's payload was nothing at all; every
       consumer branched on a `location` field no producer wrote, which is why the path-parameter registration
       and the whole request-body schema had never run.
       This row's path hole is SYMBOLIC (server-injected `state`), so the param is named and carries NO example
       — the record says the code did not compute one rather than inventing a number for it. Its body is real
       JSON, so `title` and `count` arrive as body params with the literals the code computed. */
    "fetch('/v1/users/' + state.id + '/posts', { method: 'POST',"
      " headers: { 'Content-Type': 'application/json' },"
      " body: JSON.stringify({ title: 'firstPost', count: 3 }) });"
    /* AND THE ALIGNED EXAMPLE, which is the half a shape alone cannot show. HTML §6.2 "Page visibility"'s
       state is CONCOLIC (a branch on it still forks both worlds) AND carries the value this document has,
       so the URL's shape and its computed example line up segment for segment and the path param gets
       `visible` — a value the code determined, never a solve. The query param beside it proves the three
       kinds coexist on one record. */
    "fetch('/v1/vis/' + document.visibilityState + '/reports?deep=1');"
    "(async function(){ var p = new Promise(function(res){ Promise.resolve().then(function(){ res('lazyRegion'); }); }); var c = await p; fetch('/api/lazy?r=' + c); })();"   /* PENDING await: the promise resolves LATER via a microtask (stands in for the network) -> park + resume */
    "var _spr; var _sp = new Promise(function(r){ _spr = r; }); _sp.then(function(v){ fetch('/api/shared?v=' + v); }); _spr(state.beta ? 'shBETA' : 'shSTABLE');"   /* PROBE: _sp created BEFORE the state.beta snapshot fork -> shared; settled per-flow. If promise state is NOT per-flow COW, only ONE value survives (contamination) */
    "</script>"
    /* THE FRAME CONTROL: a SHORT program of its own that asks for a reply and ends in the step that starts it.
       `flow_deliver_one_reply` is gated on `f->frame` twice (the `if (!f->frame)` around the task ladder, and
       `flow_stack_empty`, which returns 0 for a framed flow), and a frame is released only when its program
       runs to completion — so a reply asked for inside a long program waits for the rest of it. This row and
       the reply-consuming rows above are one experiment: this one at 1 with them at 0 says the gate is the
       frame.
       ITS REACHABILITY IS `deepest`, on the @COLD line: the highest program index any flow has compiled. Below
       this program's index a 0 here is the row not running; the `programCursors` bucket at that index is
       members that have finished the program before it (a cursor names the program a member LEFT), and
       `live - framed` bounds how many of them are idle. */
    /* NAMED RESIDUAL — the reply census is not partitioned by kind. Not covered: `replyAsked`/`replyAnswered`
       count RECORDS of all four kinds solver/result.c names (a fetch, an injected `<script src>`, document
       script slots, a dynamic `import()`), so a run whose every delivery served a script slot prints the same
       pair as one whose every delivery served a fetch. Next diff: split the pair by kind — the record carries
       one (solver/pending.h's `KIND`) and `pending_count_kind` reads it. Shows as: a non-zero
       `deliver-one-reply` beside zeroed reply rows with no way to tell which kind was delivered. Which kinds a
       document can produce is derived from the door (grep `engine_pending_script_url`'s callers), never by
       surveying a document's text. */
    /* NAMED RESIDUAL — §4.12.1.1's `src` ATTRIBUTE CHANGE STEPS ("If localName is src, value is not null, and
       element is connected, then run the script HTML element post-connection steps, given element") are
       reached by no page code here: `injhost` sets `src` while detached and takes the insertion door. Next
       diff: a statement that appends FIRST and assigns `src` after, served at its own TF_SERVED address. Shows
       as: every injected-script row green while a regression in the attribute-change door moves no row. */
    "<script>fetch('/api/config').then(function(r){ return r.json(); })"
            ".then(function(c){ fetch('/api/framectl?r=' + c.region); });</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
