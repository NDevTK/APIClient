/* THE NAVIGATION DOCUMENT: child navigables, cross-document reads, history and navigation API state.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_NAVIGATION_H
#define ENGINE_HOST_FIXTURE_NAVIGATION_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_NAVIGATION[] =
    "<!doctype html><html>"
    "<body>"
    /* The boot fork: `cfg.admin` forks here, so every later test of it in this document is decided inside the
       two arms that already exist rather than forking where it stands. */
    "<script>var cfg = { admin: state.admin }; if (cfg.admin) { cfg.arm = 'A'; } else { cfg.arm = 'P'; }</script>"
    "<script>"
    /* A SYNCHRONOUS READ THE HOST MUST ANSWER — the shape `iframe.contentWindow.document.body` has. The value
       arrives at the CALL SITE, on the next line, with no promise anywhere: between the two the flow suspended,
       the scheduler ran other flows, the host answered, and this flow resumed. Inside a LOOP so it happens
       repeatedly, and the concatenation is what proves each answer reached the call that asked for it rather
       than an earlier or later one. */
    "var _hr=''; for (var _i=0;_i<3;_i++) { _hr += hostRead('hr'+_i); } fetch('/api/hostreq?v=' + _hr);"
    /* AND ONE ON EACH SIDE OF A FORK. A blocked flow that forks must RE-ISSUE its request under the sibling's
       own world — sharing the id would deliver one answer into two call sites in two contradictory worlds. */
    "fetch('/api/hostreqfork?v=' + hostRead(cfg.admin ? 'hrA' : 'hrP'));"
    /* §7.4: a child navigable. `open()` hands back a WindowProxy for a document in ANOTHER instance AT ITS OWN
       CALL SITE — the child's name is minted here, so there is nothing to ask and nothing to suspend for. */
    "var _w = open(); fetch('/api/navopen?v=' + (_w ? 'proxy' : 'null'));"
    /* §7.2.5.1's SAME-ORIGIN CHECK. A popup at ANOTHER origin exposes the fixed cross-origin list and nothing
       else: `closed` answers, `name` is a SecurityError. Both halves are the assertion — a filter that threw
       for everything would pass a test that only checked the throw. */
    "var _x = open('https://other.test/p'), _sop = '';"
    "try { _x.name; _sop += 'LEAKED'; } catch (e) { _sop += e.name; }"
    "fetch('/api/sop?v=' + _sop + ':' + (_x.closed === false ? 'closedok' : 'wrong'));"
    /* §7.2.5.1 ACROSS INSTANCES: the child's document lives in another instance, so this read SUSPENDS the
       flow, the peer answers, and the flow resumes with the value AT THE CALL SITE — `false`, not undefined. */
    /* The TYPES matter as much as the values: an answer that came back as the string "false" would satisfy a
       loose check and prove only that bytes moved. `=== false` and `=== 0` say the peer's value arrived as
       itself. */
    /* §4.8.5: INSERTING an <iframe> creates a child navigable IN THE INSERTION STEPS, so `contentWindow`
       answers ON THE LINE AFTER THE APPEND — no task, no microtask, no await. The read is deliberately in the
       same statement sequence as the append: a create that had to ask the host could not satisfy it, which is
       exactly the shape the spec files assert (`document.body.appendChild(frame).contentWindow`). Reading
       THROUGH the proxy is what suspends, and the peer answers, so `closed` arrives as a real boolean. */
    /* AND THE NAVIGABLE'S OWN STATE IS ANSWERED HERE, not by the peer: self/window/frames are this navigable's
       proxy by definition, `parent`/`top` are the creator, `opener` is null for a nested one, and `name` is the
       element's `name` ATTRIBUTE — which the removal below then empties while leaving the attribute alone. */
    /* Each claim is one character of `_ifb`, `1` held, `0` not, `E` threw, so a failure names its claim. */
    "var _ifb = ''; function _ift(f) { try { _ifb += f() ? '1' : '0'; } catch (e) { _ifb += 'E'; } }"
    "var _if = document.createElement('iframe'); _if.setAttribute('name','fr');"
    "document.body.appendChild(_if); var _cw = _if.contentWindow;"
    "_ift(function(){ return !!_cw; }); _ift(function(){ return _cw.self === _cw; });"
    "_ift(function(){ return _cw.window === _cw; }); _ift(function(){ return _cw.frames === _cw; });"
    "_ift(function(){ return _cw.globalThis === _cw; }); _ift(function(){ return _cw.parent === window; });"
    "_ift(function(){ return _cw.top === window; }); _ift(function(){ return _cw.opener === null; });"
    "_ift(function(){ return _cw.closed === false; }); _ift(function(){ return _cw.name === 'fr'; });"
    /* AND ONE READ THAT REACHES THE ACTIVE DOCUMENT, which is what makes the removal below a RECLAMATION probe
       rather than a flag probe. Every member above is answered from the navigable's own record, so none of them
       builds anything: without this line the srcless child stays unmaterialized for the whole fixture and the
       destroy has no realm to let go of. `document` forwards to the child's Window, so the initial about:blank
       Document and its realm exist from here on — ONE PER FLOW, which is the shape the ceiling is made of. */
    "var _cwDoc = null;"
    "_ift(function(){ _cwDoc = _cw.document; return !!_cwDoc && _cwDoc !== document; });"
    /* §4.8.5's REMOVING STEPS: the element loses its navigable, and the proxy the page still holds stays the
       same object while reporting a destroyed one. */
    "document.body.removeChild(_if);"
    "_ift(function(){ return _if.contentWindow === null; }); _ift(function(){ return _cw.closed === true; });"
    "_ift(function(){ return _cw.name === ''; }); _ift(function(){ return _cw.self === _cw; });"
    "_ift(function(){ return _if.getAttribute('name') === 'fr'; });"
    /* AND THE PROXY THE PAGE IS STILL HOLDING STILL ANSWERS ITS WINDOW'S DOCUMENT. HTML §7.2.2 "The Window
       object": "The document getter steps are to return this's associated Document", and §7.5.10's note says
       "even after destruction, the Document object itself might still be accessible to script, in the case
       where we are destroying a child navigable" — `_cw` is that reference, so it reads the same Document. The
       `name` claim above is the same section's "If this's navigable is null, then return the empty string."
       A failure carries what the engine answered for both. */
    "_ift(function(){ return _cw.document === _cwDoc; });"
    "var _ifv = function(f){ try { var v = f(); return v === _cwDoc ? 'samedoc' : v === null ? 'null'"
    " : typeof v === 'string' ? 's' + v : typeof v; } catch (e) { return 'E' + e.name; } };"
    "fetch('/api/iframenav?v=' + (/^1+$/.test(_ifb) ? 'ifnav' : 'claims' + _ifb"
    " + '-name' + _ifv(function(){ return _cw.name; }) + '-doc' + _ifv(function(){ return _cw.document; })));"
    /* NOT PROBED: an iframe with a REAL `src` (§7.4 step 14's load job, which parks on the host for the
       response and runs the child's scripts); `node engine/wpt.mjs html/browsers` runs those. A srcless
       navigable that something reads through is materialized exactly as a src'd one is, so this costs one
       child realm per flow with no load in it, and removing the iframe exercises §7.5.10 step 7 — the
       navigable letting go of the Document, the one counted reference to a child realm
       (core/frame/window_proxy.h). Whether the realm comes back is the `realm-reclaim` row, which compares
       `childRealmsMade` against `childRealmsPeak`. */

    /* A fetch taken from a removed iframe's window, as Sentry takes its transport (it reads a fresh
       hidden iframe's `contentWindow.fetch`, removes the iframe and binds the function to this window). Chrome
       155 sends it, keepalive or not, as this window's request: its referrer is this document's address and a
       relative URL resolves against this document. Fetch §5.6 "Fetch methods" reads the receiver's realm
       ("Let relevantRealm be this's relevant realm"), so the response is this realm's `Response`. The same
       function called with the removed window as its receiver rejects with a TypeError in Chrome 155. The
       fixture host echoes each request's referrer, and this document's own `fetch` is the control. */
    "var _sbI = document.createElement('iframe'); _sbI.hidden = true; document.head.appendChild(_sbI);"
    "var _sbW = _sbI.contentWindow, _sbF = _sbW.fetch; document.head.removeChild(_sbI);"
    "var _sbO = { method: 'POST', body: 'x', keepalive: true, referrerPolicy: 'unsafe-url' };"
    "Promise.all([fetch('/api/sbxctl', _sbO), _sbF.bind(window)('/api/sbxsend', _sbO)]).then(function (rs) {"
    " var a = rs[0].headers.get('x-echo-referrer'), b = rs[1].headers.get('x-echo-referrer');"
    " fetch('/api/sbxbound?v=' + (a === b ? 'same' : 'differ') + '&m=' + rs[1].headers.get('x-echo-method')"
    " + '&realm=' + (rs[1] instanceof Response ? 'this' : 'other'));"
    "}, function (e) { fetch('/api/sbxbound?v=rejected' + e.name); });"
    "_sbF.call(_sbW, '/api/sbxdead', { method: 'POST', body: 'x' }).then(function () {"
    " fetch('/api/sbxdetached?v=resolved'); }, function (e) { fetch('/api/sbxdetached?v=' + e.name); });"

    /* HTML §7.4.4 "Non-fragment synchronous \"navigations\""' URL AND HISTORY UPDATE STEPS, reached as every
       client-side router reaches them: §7.2.5 "The History interface"'s `pushState` with a url — the one
       statement that makes a document nothing navigates to (solver/route_seed.c seeds it).
       THE ROUTE IS COMPUTED OUT OF A REPLY: `us-west-2` is a field of the JSON this host serves, so
       `/routes/us-west-2/admin` is in no source text and only running the concatenation produces it.
       BOTH ENDS ARE READ: `location.pathname` is §7.4.4 step 8 having moved this Document's address, and the
       C side checks the notice's address and provenance (`g_seed_route_derived`).
       AND IT PUTS THE ADDRESS BACK with `replaceState`: §8.1.3.2 "Environment settings objects"' API base URL
       is the document's address, so a route left in place would resolve every later reference against
       `/routes/us-west-2/`. */
    "(async function(){ var _rr = (await (await fetch('/api/config')).json()).region;"
    " var _route = '/routes/' + _rr + '/admin';"
    " history.pushState(null, '', _route);"
    " fetch('/api/routeseed?v=' + (location.pathname === _route ? 'seeded' : 'wrong'));"
    " history.replaceState(null, '', '/p'); })();"
    /* §7.2.5 "The History interface"'s `pushState` AND §7.2.6.6's `updateCurrentEntry`, each handed a value
       CONTAINING AN UNKNOWN. A session history entry outlives the turn, so the triple rides AS DATA beside
       the bytes (§7.4.1.1's entry carries the second slot) or the value stands for its example alone and a
       branch on it is DECIDED.
       THE WITNESS IS A FORK: `?v=fork` is learned only if the arriving value carried its domain, `?v=other`
       under either reading — the armed control.
       `.slice(1)` because a non-empty hash always begins with `#`, so `=== 'admin'` over the hash itself is
       infeasible and pruned correctly.
       THE URL IS THIS DOCUMENT'S OWN ADDRESS, so §7.4.4 step 8 moves nothing, and no row reads
       `history.length`. The async body has no `await`: it changes no tempo and contains a TypeError from a
       null `history.state` or `navigation.currentEntry`, which the ladder's bottom rung reports. */
    "(async function(){"
    " history.pushState({t: location.hash.slice(1)}, '', '/p');"
    " if (history.state.t === 'admin') fetch('/api/hstate?v=fork');"
    " else fetch('/api/hstate?v=other');"
    " navigation.updateCurrentEntry({state: location.hash.slice(1)});"
    " if (navigation.currentEntry.getState() === 'admin') fetch('/api/navstate?v=fork');"
    " else fetch('/api/navstate?v=other'); })();"

    /* THE SAME READ FROM INSIDE A JOB. A `.then` handler is a queued reaction, and a cross-document read
       SUSPENDS — so this exercises a step machine that parks on the host while it is the root of a job rather
       than reached from a bytecode frame. Without the scheduler reporting that flow host-owed, it resumes and
       parks forever while every turn looks like progress, the host is never asked, and the run livelocks. */
    "Promise.resolve().then(function(){"
    " fetch('/api/xdocjob?v=' + (_w.closed === false ? 'jobread' : 'wrong')); });"
    "fetch('/api/xdocread?v=' + (_w.closed === false && _w.length === 0 &&"
    " typeof _w.closed === 'boolean' && typeof _w.length === 'number' ? 'xread' : 'wrong'));"
    "</script>"
    /* HTML §7.4.2.2 "Beginning navigation"'s destination as an @H row (the `navigation` door). This host
       answers every fetch with `{"region":"us-west-2"}`, so the `!==` arm contradicts the reply: `/navhidden`
       is a page only a forced arm opens, the shape `if (user.isAdmin) open('/admin')` has. `/navpage` is
       opened by this document's own unforked code, and `other.test` is cross-site, so it is announced to a
       peer rather than loaded here; the `nav-door` row reads all three. */
    "<script>fetch('/api/navgate').then(function(r){ return r.json(); }).then(function(c){"
    " if (c.region !== 'us-west-2') open('/navhidden'); });"
    "open('/navpage'); open('https://other.test/navxsite');</script>"
    /* A navigation response's Set-Cookie, in the asking flow's jar only (Fetch §4.7 "HTTP-network fetch"). The
       `!==` arm contradicts the reply and opens `/cookieload`, whose answer sets `flowc` and an HttpOnly `hoc`;
       its Document reads document.cookie. The other arm waits three host round trips and reads its own; the
       `flow-jar` row reads both. */
    /* The root's own response set `rootc` (Path=/p) before this document ran, received into the agent's jar at
       the baseline as main.c's qjs_receive_cookies hands an instance its root's fields; the `root-jar` row
       reads it here, and the `flow-jar` row's sibling arm reads it too while `/cookieload` (another path)
       does not. */
    "<script>fetch('/api/rootjar?c=' + encodeURIComponent(document.cookie || 'none'));</script>"
    "<script>fetch('/api/ckgate').then(function(r){ return r.json(); }).then(function(c){"
    " if (c.region !== 'us-west-2') { open('/cookieload'); return; }"
    " fetch('/api/ckwait').then(function(){ return fetch('/api/ckwait'); }).then(function(){"
    " return fetch('/api/ckwait'); }).then(function(){"
    " fetch('/api/cookiesib?c=' + encodeURIComponent(document.cookie || 'none')); }); });</script>"
    /* A request made from an initial about:blank Document (HTML §7.3.1 "Creating a new browsing context"), whose
       realm runs on the creating flow. The `=== 'eu-west-9'` arm contradicts the reply and pins `region`, so its
       request rests on a value the fork chose and must state `pinned`; the other arm's states `unpinned`. The
       `blank-pin` row reads both pending lines. Absolute addresses keep the about:blank base URL out of it. */
    "<script>var _bf = document.createElement('iframe'); document.body.appendChild(_bf);"
    "fetch('/api/blankgate').then(function(r){ return r.json(); }).then(function(c){"
    " if (c.region === 'eu-west-9') _bf.contentWindow.fetch('https://x.test/api/blankpin?r=' + c.region);"
    " else _bf.contentWindow.fetch('https://x.test/api/blankobs?r=' + c.region); });</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
