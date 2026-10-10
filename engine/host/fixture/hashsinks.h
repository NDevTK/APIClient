/* THE FRAGMENT SINKS DOCUMENT: `location.hash` into a script, markup, attribute and URL sink, and JSON.parse's outcome fork.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_HASHSINKS_H
#define ENGINE_HOST_FIXTURE_HASHSINKS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_HASHSINKS[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* THE SOURCE'S BROWSER TRANSFORM decides whether a breakout is real, and the SAME candidate through the
       SAME source answers differently per sink context. A fragment percent-encodes `<` but NOT the apostrophe,
       so:
         - a JS-context sink fed from location.hash IS a real XSS (`';X9()//` arrives intact), and
         - an HTML-context sink fed from the same source is NOT (the `<` arrives as %3C and parses as text).
       Before the delivery transform existed the solver handed the payload over raw and would report BOTH as
       working, which is a false PoC — the thing this half of the engine must never produce.
       BOTH HALVES ARE ASSERTED, through ONE source into TWO sink contexts, which is the only arrangement that
       can tell "the transform is applied" apart from "the solver is failing to solve": the apostrophe survives
       the fragment set so the JS sink FIRES (through `'#';X9()//'` — the leading `#` included), while `<` does
       not survive so the HTML sink CANNOT, and it is reported as a PARKED SEARCH rather than omitted. The
       negative half costs the run TWO extra candidate re-fires that are known not to fire — the markup sink's
       context PROBE and the one escape solve_html.c derives from it — where the deleted CANDS_HTML spray cost
       five; that cost buys the one property the @S half must never lose, so it is paid on every build. */
    "eval(\"'\" + location.hash + \"'\");"
    "var lhHost = document.createElement('div'); document.body.appendChild(lhHost);"
    "lhHost.innerHTML = location.hash;"
    /* AND THE SAME SOURCE INTO AN ATTRIBUTE-VALUE HOLE — §@S's observations solved JOINTLY. The write above
       lands in §13.2.5.1's data state, whose only exit is `<`, which the fragment percent-encode set (URL §1.3
       "Percent-encoded bytes") holds, so that search is unsolvable. This one lands in §13.2.5.37 "Attribute
       value (single-quoted) state", left by the apostrophe the set does not hold, into §13.2.5.39 "After
       attribute value (quoted) state", left by whitespace and by U+002F SOLIDUS, which no set holds. So an
       escape exists with no `<`, `>` or space: a derivation reading only the sink's parse context constructs
       a spelling the source cannot carry and misses a real raw-fragment XSS. The first write must NOT fire and
       this one MUST.
       A DERIVED SOURCE IDENTITY ON PURPOSE: `location.hash.slice(1)` composes `{location.hash}.slice(1)`
       (argument included), so this is its own search and the negative half keeps its parked entry; the root
       is still `location.hash`, which carries the percent-encode set through the derivation. */
    "var lhAttr = document.createElement('div'); document.body.appendChild(lhAttr);"
    "lhAttr.innerHTML = \"<img alt='\" + location.hash.slice(1) + \"'>\";"
    /* AND THE SAME SOURCE INTO THE URL CONTEXT, the third of the three, whose class has one vector —
       solver/solve.h: "navigating executes the `javascript:` scheme and nothing else does". The fragment set
       holds space, `"`, `<`, `>` and the backtick and none of `javascript:X9()`, so this one is solvable
       through a raw fragment: the positive half of the pair in a third context.
       §4.6.5 "Following hyperlinks" is one of the doors onto the URL sink, beside §7.2.4's three whole-URL
       algorithms, §7.2.2.1's `url` argument and §4.10.22.3's action, and must announce through
       solve_url_sink as they do.
       THE CLICK IS `click()` AND NOT A DISPATCHED `Event`: DOM §2.9 step 6.4 makes an event an activation
       event only when it is a MouseEvent whose type is `click`, so `dispatchEvent(new Event('click'))` runs no
       activation behaviour; HTML §6.5's `click()` fires a synthetic pointer event carrying that brand.
       THE WITNESS FETCH CARRIES CONSTANTS ONLY, because a payload composed from a concolic can itself be
       concolic, and the request would then never be made. */
    "var lhLink = document.createElement('a');"
    "lhLink.href = location.hash.slice(1);"
    "document.body.appendChild(lhLink);"
    "lhLink.click();"
    "fetch('/api/aclick');"
    /* THE OUTCOME FORK AT A C BUILTIN. `JSON.parse` of unknown text has two feasible completions — §25.5.2
       JSON.parse ( text [ , reviver ] ) step 2's normal completion and the SyntaxError of §25.5.2.1 ParseJSON
       ( text ) step 1 — and a builtin that picks one has DELETED the arm the `catch` and everything behind it
       lives on. BOTH endpoints below must appear in ONE run: two flows, one snapshot-forked from the other AT
       the builtin. (The numbers stood as `25.5.1 step 8` and `step 2's SyntaxError`, which name a section that
       is now JSON.isRawJSON and a step JSON.parse's own list no longer has: the throw moved out into the
       ParseJSON abstract operation. Read off the standard's text rather than recalled.)
       AND THE REFINEMENT, which is the negative half and the reason this is two statements and not one: the
       SAME builtin over `location.hash` ITSELF has only ONE feasible completion, because the component that
       owns that source declares what the browser delivers — the empty string or `#` followed by the fragment,
       neither of which is a JSON text. So that one must throw with NO fork, exactly as V8 does, and
       /api/jsonrawok must never be reached. Asserting only the fork would pass with the refinement absent;
       asserting only the throw would pass with the fork absent. */
    "try { JSON.parse(location.hash.slice(1)); fetch('/api/jsonok'); }"
    "catch (jpe) { fetch('/api/jsonthrew?n=' + jpe.name); }"
    "try { JSON.parse(location.hash); fetch('/api/jsonrawok'); }"
    "catch (jpe2) { fetch('/api/jsonrawthrew?n=' + jpe2.name); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
