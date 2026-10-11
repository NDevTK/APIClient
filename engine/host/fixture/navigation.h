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
    "_ift(function(){ return !!_cw.document && _cw.document !== document; });"
    /* §4.8.5's REMOVING STEPS: the element loses its navigable, and the proxy the page still holds stays the
       same object while reporting a destroyed one. */
    "document.body.removeChild(_if);"
    "_ift(function(){ return _if.contentWindow === null; }); _ift(function(){ return _cw.closed === true; });"
    "_ift(function(){ return _cw.name === ''; }); _ift(function(){ return _cw.self === _cw; });"
    "_ift(function(){ return _if.getAttribute('name') === 'fr'; });"
    /* AND THE PROXY THE PAGE IS STILL HOLDING HAS NO ACTIVE DOCUMENT — HTML §7.5.10's own note ("even after
       destruction, the Document object itself might still be accessible to script, in the case where we are
       destroying a child navigable") is about a reference the PAGE kept, and `_cw` is exactly that reference.
       Step 9 nulled the navigable's active document, so this read must not reach one AND must not build one:
       a proxy whose realm slot is empty because it was released reads identically to one that has never been
       materialized, and only the destroyed flag tells them apart (window_proxy.c's proxy_realm asserts it).
       The `undefined` is §7.2.3's own surface answering a name it does not carry, which is this engine's
       pre-existing answer for every through-read on a closed navigable and not a value this probe chose. */
    "_ift(function(){ return _cw.document === undefined; });"
    "fetch('/api/iframenav?v=' + (/^1+$/.test(_ifb) ? 'ifnav' : 'claims' + _ifb));"
    /* NOT PROBED: an iframe with a REAL `src` (§7.4 step 14's load job, which parks on the host for the
       response and runs the child's scripts); `node engine/wpt.mjs html/browsers` runs those. A srcless
       navigable that something reads through is materialized exactly as a src'd one is, so this costs one
       child realm per flow with no load in it, and removing the iframe exercises §7.5.10 step 7 — the
       navigable letting go of the Document, the one counted reference to a child realm
       (core/frame/window_proxy.h). Whether the realm comes back is the `realm-reclaim` row, which compares
       `childRealmsMade` against `childRealmsPeak`. */

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
    TF_CANVAS_INK
    "</body></html>";

#endif
