/* THE FRAGMENT SINKS DOCUMENT: `location.hash` into a script, attribute and URL sink, and JSON.parse's outcome fork.
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
       The JS sink FIRES (through `'#';X9()//'` — the leading `#` included). The markup sink that cannot is in
       its own document (fixture/hashpark.h): here, a raw-fragment markup write would share its search with
       `lhAttr` below and rightly fire there. */
    "eval(\"'\" + location.hash + \"'\");"
    /* AND THE SAME SOURCE INTO AN ATTRIBUTE-VALUE HOLE — §@S's observations solved JOINTLY. This lands in
       §13.2.5.37 "Attribute value (single-quoted) state", left by the apostrophe the fragment percent-encode
       set (URL §1.3 "Percent-encoded bytes") does not hold, into §13.2.5.39 "After attribute value (quoted)
       state", left by whitespace and by U+002F SOLIDUS, so an escape exists with no `<`, `>` or space. And
       the one delivered value is also what the eval above reads, so the escape must leave that eval parsing:
       the separator is §12.4's `//`, which the markup tokenizer also reads as one (solver/solve_joint.h).
       `location.hash.slice(1)` composes `{location.hash}.slice(1)`, its own search; its root is still
       `location.hash`, which carries the percent-encode set through the derivation. */
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
