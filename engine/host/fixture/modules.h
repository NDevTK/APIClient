/* THE MODULES DOCUMENT: module scripts, dynamic import(), a static graph, and an injected chunk.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MODULES_H
#define ENGINE_HOST_FIXTURE_MODULES_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MODULES[] =
    "<!doctype html><html>"
    "<body>"
    /* The boot fork: `cfg.admin` forks here, so every later test of it in this document is decided inside the
       two arms that already exist rather than forking where it stands. */
    "<script>var cfg = { admin: state.admin }; if (cfg.admin) { cfg.arm = 'A'; } else { cfg.arm = 'P'; }</script>"
    "<script>"
    /* A TOP-LEVEL `const` READ FROM ITS OWN DEFAULT PARAMETER, IN A CONTINUATION — the shape rolldown/vite
       bundles ship (`const __vite__mapDeps=(i,m=__vite__mapDeps,d=...)=>i.map(i=>d[i])`, which resolves a
       dynamic import's dependency list and is read from inside an `async` function).
       ECMAScript puts the default on the CALL: §10.2.11 FunctionDeclarationInstantiation evaluates a formal
       parameter's initializer as part of the call, by which time §14.3.1 Let and Const Declarations has
       initialised the outer binding — so `m === mdSelf` is true, and `is not initialized` would be this engine
       reading a cell its own timeline initialised as though it had not.
       The token is a literal: `mdOK` against `mdWRONG` says which arm the comparison took; the absence of
       both says the read threw. */
    "const mdSelf = (i, m = mdSelf) => (m === mdSelf ? 'mdOK' : 'mdWRONG'); (async function(){ await 0; fetch('/api/selfdefault?w=' + mdSelf([0])); })();"
    /* …AND THE SAME READ ON A FORKED PATH: both arms must read `mdfOK`, because the binding is the PROGRAM's and
       neither world wrote it.
       THE PER-WORLD REACH EMISSION IS AHEAD OF THE CLAIM: `cfg.admin` forks once per document (later tests are
       decided inside the arms that exist), so one arm here can be the SCHEDULE rather than the fork. The reach
       token carries `cfg.admin`'s arm letter only, so two records say two worlds ran the continuation, and
       `/api/mdforkpath` with one arm beside both reach letters is the claim failing. */
    "const mdFork = (i, m = mdFork) => (m === mdFork ? 'mdfOK' : 'mdfWRONG'); (async function(){ await 0; fetch('/api/mdforkreach?w=' + (cfg.admin ? 'A' : 'P')); fetch('/api/mdforkpath?w=' + (cfg.admin ? 'A' : 'P') + mdFork([0])); })();"
    /* …AND THE SAME TWO READS AT MODULE TOP LEVEL, THROUGH A DYNAMIC `import()`. A script's lexical bindings and
       a module's are different cells written by different algorithms — a module's inside §16.2.1.6.1.3
       Evaluate ( )'s own body after the link phase, the pair `solver/cow.c`'s `module_eval` hook isolates.
       THE ENTRY WITNESS IS THE FIRST FETCH: `mmENTER` fires before the await, so no `mmENTER` means the
       statement did not run, `mmENTER` alone means the import was never settled, and `mmENTER` with either of
       the others means the module evaluated and the reads are measured. Every token is a literal. */
    "(async function(){ fetch('/api/modentered?w=mmENTER'); var ns = await import('/chunk/mdmod.js'); fetch('/api/modconst?w=' + ns.mmProbe()); fetch('/api/moddefault?w=' + (ns.default && ns.default.tag ? ns.default.tag : 'mmNODEF')); })();"
    /* …AND THE SAME MODULE BINDING READ FROM TWO WORLDS. A module is LINKED once and its status is a field of
       a C record, while the cells its body writes go through the page's write hooks and are PER-FLOW — so a
       world that did not evaluate it finds the status saying there is nothing to do and the cells saying
       nothing was done.
       `cfg.admin` IS THE SECOND WORLD AND NOT A NEW FORK: the boot fork above made both worlds before the
       import, so each arm is an existing world; a fresh branch after the import would leave both arms
       descending from the world that evaluated it.
       THE REACH EMISSION CARRIES NO MODULE READ: `mmProbe()` throws on an uninitialised cell and loses its whole
       request, so two reach letters with one `mmOK` is the defect and one reach letter is the schedule. */
    "(async function(){ var mw = await import('/chunk/mdmod.js'); fetch('/api/modworldreach?w=' + (cfg.admin ? 'A' : 'P')); fetch('/api/modworld?w=' + (cfg.admin ? 'A' : 'P') + mw.mmProbe()); })();"
    /* —AND THE SAME TWO-WORLD READ WITH NO MODULE IN IT, which is what a minified bundle's inlined
       `__commonJS` is: a CLOSURE OVER A CACHE CELL whose factory writes a PROPERTY ON THE OBJECT IT CACHED. The
       observed defect is the cell truthy while that property reads undefined — the two writes isolated from
       each other. `c = { exports: {} }` writes a closure cell and `m.exports = { tag: ... }` a property slot,
       spelled apart so they can disagree; the `||` skips the factory in a world that sees the cell filled.
       No module, import, served body or network: ordinary script through the ordinary write hooks.
       `cfg.admin` IS THE SECOND WORLD: the boot fork made both worlds before the first `_cjg()`, so each world
       calls the factory itself.
       THE REACH EMISSION IS FIRST AND READS NEITHER WRITE; its letters are the arms', so two records say two
       worlds ran. Tokens are literals and no token is a prefix of another.
       IT CANNOT THROW: `_cj && _cj.tag === 'cjHELD'` never touches a property of undefined, so the defect is the
       token `cjLOST`, with both reach letters present. */
    "var _cjg = (function(){ var c; function f(m){ m.exports = { tag: 'cjHELD' }; }"
    " return function(){ return c || (f(c = { exports: {} }), c.exports); }; })();"
    "fetch('/api/cjreach?w=' + (cfg.admin ? 'A' : 'P'));"
    "var _cj = _cjg();"
    "fetch('/api/cjworld?w=' + (cfg.admin ? 'A' : 'P')"
    " + (_cj && _cj.tag === 'cjHELD' ? 'cjOK' : 'cjLOST'));"
    /* …AND THE GATE THAT DECIDES WHETHER THOSE BYTES ARE A PROGRAM AT ALL, asked of a module served
       `application/json` whose body is VALID JavaScript. HTML §8.1.4.2's fetch a single module script extracts a
       MIME type and leaves `moduleScript` null for anything that is not a JavaScript MIME type, and
       §8.1.6.7.3 HostLoadImportedModule's onSingleFetchComplete makes a null moduleScript a TypeError — so the
       LOAD rejects and `mbRAN` is unreachable. THE THREE TOKENS ARE THREE DIFFERENT ENGINES: `mtASK` alone says
       the import never settled; `mtASK` with `TypeError` is this gate refusing the bytes; `mtASK` with
       `mtLOADED` AND `mbRAN` is an engine that compiled a JSON reply as a module, which is what this document
       measured before the gate existed. `SyntaxError` would be a third — a compile of bytes that did not parse —
       and is why the control's body parses. */
    "(async function(){ fetch('/api/modtypeask?w=mtASK'); try { var bad = await import('/chunk/mdbad.js'); fetch('/api/modtype?w=' + (bad ? 'mtLOADED' : 'mtEMPTY')); } catch (e) { fetch('/api/modtype?w=' + e.name); } })();"
    "</script>"
    "<script type=module>"
    " fetch('/api/modreach?d=ok');"
    " fetch('/api/modtla?w=resumed');"
    "</script>"
    /* WEB CRYPTOGRAPHY §29.4.5 AES-GCM EXPORT KEY — `generateKey` then `exportKey` in three formats, the
       reachability witness for §14.3.6 and §14.3.10.
       EVERY EMITTED PAYLOAD IS A CONSTANT: `kty`, `alg` and the `key_ops` members are literals this engine wrote
       at §29.4.5's sub-steps, `ext` is the boolean passed in, and the raw arm's length is asked as a comparison
       against a literal. The key material is never emitted: core/crypto/crypto.h's stream is reproducible today,
       but a witness carrying key bytes would go silent the day that source is device-backed.
       THE CHANNEL IS THE RESULT DOCUMENT: the renderer does not tee stdout, so a console scrape cannot tell
       "cannot see" from "did not run".
       EVERY RECORD IS BEHIND AN `await`, and §14.3.6 and §14.3.10 settle through sc_resolve's
       JS_EnqueueCallJob, so these rows are answered by the JOB pump: read the job counter before reading a 0
       as a defect — work done is the wrong denominator for anything behind an await. */
    "<script>"
    "(async function(){ var _xk;"
    " try { _xk = await crypto.subtle.generateKey({name:'AES-GCM',length:256}, true, ['encrypt','decrypt']); }"
    " catch (_e) { fetch('/api/xkreach?d=no&n=' + _e.name); return; }"
    " fetch('/api/xkreach?d=ok');"
    " try { var _r = await crypto.subtle.exportKey('raw', _xk);"
    "       fetch('/api/xkraw?d=' + (_r.byteLength === 32 ? 'ok' : 'wrong')); }"
    " catch (_e) { fetch('/api/xkraw?d=' + _e.name); }"
    " try { var _j = await crypto.subtle.exportKey('jwk', _xk);"
    "       fetch('/api/xkjwk?kty=' + _j.kty + '&alg=' + _j.alg + '&ext=' + _j.ext"
    "             + '&ops=' + _j.key_ops.join('.')); }"
    " catch (_e) { fetch('/api/xkjwk?kty=' + _e.name); }"
    " try { await crypto.subtle.exportKey('spki', _xk); fetch('/api/xkspki?d=resolved'); }"
    " catch (_e) { fetch('/api/xkspki?d=' + _e.name); }"
    "})();"
    "</script>"

    /* HTML §4.12.1.1 "Processing model" OVER A CONNECTED `<script src>` — the one producer of an injected-script
       park (`FLOW_PENDING_SCRIPT`; solver/engine.c's `engine_pending_script_url`), reached from `prepare the
       script element` over an element carrying a `src`. The host's `loadScript` edge parks nothing, and an
       element never inserted is refused by §4.12.1.1's "If el is not connected, then return".
       `src` THEN APPEND: on a detached element the `src` attribute change steps return at their own "element
       is connected" test, so the APPEND prepares (core/dom/element.c's insertion walk) — the order a bundler
       writes. The mirror order is the named residual at the frame control in the fetch document.
       NOTHING SETS `async`: a `createElement`'d script's force async stays true ("It is set to false by the HTML
       parser and the XML parser on script elements they insert, and when the element gets an async content
       attribute added"), so scheduling takes `SCRIPT_SCHED_ASAP`, the destination that reaches this park.
       `s.async = false` would take the in-order list (`FLOW_PENDING_DOCSCRIPT`, which HTML_COLD reaches).
       The address is served as a program by TF_SERVED, not TF_CHUNKS (`loadScript`'s, which parks nothing).
       The payload paths are literals and the `src` readback rides a param: `d=ok` says the attribute is present
       before the append, so a `src` setter that failed to reflect cannot read as a park that never happened. */
    "<script id=injhost>"
    "(function(){ var s = document.createElement('script');"
    " s.setAttribute('id', 'injel');"
    " s.src = '/chunk/injsrc.js';"
    " fetch('/api/injmade?d=' + (s.getAttribute('src') === '/chunk/injsrc.js' ? 'ok' : 'wrong'));"
    " document.body.appendChild(s); })();"
    "</script>"
    /* ─── §16.2.1.7.3.1 "InitializeEnvironment ( )" — A MODULE'S TOP-LEVEL FUNCTION DECLARATION READ FROM
       INSIDE TWO NESTED CLOSURES OF A LAZILY-INVOKED FACTORY: the shape of an `@swc/helpers` shim in a
       rolldown/esbuild ESM chunk, the helper named two FunctionExpression levels below the module body.
       §16.2.1.7.3.1's lexical pass runs InstantiateFunctionObject and InitializeBinding for a
       FunctionDeclaration BEFORE §16.2.1.7.3.2 ExecuteModule, so the binding never holds a pre-instantiation
       value from any depth; `d=Fok` is that sentence as an observation. `mfd('ok')` runs over a literal, so
       the result is concrete.
       THE `typeof` ARM: a var-shaped binding answers `undefined` and emits it; a lexical one in its dead zone
       throws (§13.5.3 "The typeof Operator" step 2.a answers only for an unresolvable Reference), and the
       statement emits nothing.
       TWO ROWS, ENTAILED: `mfd-top` reads the binding at the module body's level before the thunk runs and is
       the reachability witness, so `mfd-nest` at 0 beside `mfd-top` at 1 is the closure-variable chain
       (get_closure_var) and not the cell's initialization. */
    "<script type=module>"
    " function mfd(x){ return 'F' + x; }"
    " fetch('/api/mfdtop?d=' + (typeof mfd === 'function' ? 'ok' : typeof mfd));"
    " var mfdlazy = (function(fac){ return fac; })(function(a, b){"
    "   (function(c, d){ d(); })(b, function(){"
    "     fetch('/api/mfdnest?d=' + (typeof mfd === 'function' ? mfd('ok') : typeof mfd)); });"
    " });"
    " mfdlazy(1, 2);"
    "</script>"
    /* ─── THE ONE STATEMENT THAT MAKES THIS DOCUMENT'S MODULE GRAPH MORE THAN ONE NODE ──────────────
       See the served `mdhost.js` body for why a static `import` is a different subsystem from the dynamic rows.
       THE ENTRY WITNESS IS THE FIRST EMISSION: `mhASK` is unconditional and ahead of the `await`, so no row
       behind it is read as a module finding until it answers.
       THE `catch` SPLITS A SHUT LADDER: `mh-ask` at 1 with `mh-dep` at 0 is either a rejected load or one that
       never settled, and the reject arm names which. `e.name` is a string the engine wrote from its own table,
       and the row reading it is NEGATIVE, banded by its `reach`.
       The done token is a literal and no row: everything it could claim the importer's own emissions claim. */
    "<script>"
    "(async function(){ fetch('/api/mhask?w=mhASK');"
    " try { await import('/chunk/mdhost.js'); fetch('/api/mhdone?w=mhDONE'); }"
    " catch (mhE) { fetch('/api/mhfail?w=' + mhE.name); } })();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
