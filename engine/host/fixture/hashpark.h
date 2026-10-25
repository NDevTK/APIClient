/* THE RAW-FRAGMENT MARKUP DOCUMENT: `location.hash` into two markup writes neither of which the fragment
   percent-encode set (URL §1.3 "Percent-encoded bytes") lets an escape leave, so that search must park and
   say why. A candidate is delivered once at its source and every read answers it, so the search's one value
   reaches both writes, and both are its contexts:
     - `lhHost` takes the raw fragment into §13.2.5.1's data state, left only through `<`;
     - `lhQuoted` takes `location.hash.slice(1)` into §13.2.5.36 "Attribute value (double-quoted) state",
       left only through `"`. The set holds both bytes.
   `lhOther`, fed from `state.other`, is the negative control of the credit: in a `location.hash` candidate
   run it carries none of that search's bytes, and its own search fires.
   It is its own document because a write whose escape the fragment can carry (hashsinks' `lhAttr`) would be
   one more context of the same search, and that search would then rightly fire there.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_HASHPARK_H
#define ENGINE_HOST_FIXTURE_HASHPARK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_HASHPARK[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var lhHost = document.createElement('div'); document.body.appendChild(lhHost);"
    "lhHost.innerHTML = location.hash;"
    "var lhQuoted = document.createElement('div'); document.body.appendChild(lhQuoted);"
    "lhQuoted.innerHTML = '<img alt=\"' + location.hash.slice(1) + '\">';"
    "var lhOther = document.createElement('div'); document.body.appendChild(lhOther);"
    "lhOther.innerHTML = \"<b title='\" + state.other + \"'>\";"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
