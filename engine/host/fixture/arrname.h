/* THE ARRAY-NAME DOCUMENT: a stored record's member reduced over an empty array literal, then enumerated, as
   Glean's events store does on mdn. With no stored value the record is an unknown with no example, so the
   reduction's result is too, and its own keys are ECMAScript §7.3.23 EnumerableOwnProperties' step 1
   [[OwnPropertyKeys]] over input. That question carries the record's identity, which is composed from the
   reduce call's operands, so the `[]` operand needs a creation name. One of test_forced.c's exploring
   documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ARRNAME_H
#define ENGINE_HOST_FIXTURE_ARRNAME_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ARRNAME[] =
    "<!doctype html><html><body>"
    "<script>"
    "var s = JSON.parse(localStorage.getItem('events') || '{}'), r = s.events;"
    "var acc = Array.isArray(r) ? r.reduce(function (t, e) { return t.concat([e]); }, []) : [];"
    "fetch('/api/arrname?n=' + Object.keys(acc).length);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
