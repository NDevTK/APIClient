/* THE TYPE FACET DOCUMENT: ECMAScript §6.1's language type of a value derived from server-injected state, read
   by §13.5.3.1 typeof. One of test_forced.c's exploring documents (its TF_DOCS table). `__DEV__` and
   `window.__CFG__` name no standard's member, so each read is an unknown (absent state, forked). Logical NOT
   answers a Boolean whatever its operand (§13.5.7), unary plus a Number (§13.5.4.1) and a concatenation with a
   String a String (§13.15.3 step 1.c), so on every arm `typeof !!__DEV__` is "boolean", `typeof +__CFG__` is
   "number" and `typeof ("a" + __CFG__)` is "string", and the Boolean `!!__DEV__` is `true` or `false`. This is
   the shape of Apollo Client's `__DEV__` invariant. It ends with TF_CANVAS_INK, which test_forced.c defines
   before including it. */
#ifndef ENGINE_HOST_FIXTURE_TYPEFACET_H
#define ENGINE_HOST_FIXTURE_TYPEFACET_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TYPEFACET[] =
    "<!doctype html><html>"
    "<body>"
    "<script>var s = (function () { try { return !!__DEV__; } catch (e) { return false; } })();"
    "var c = window.__CFG__;"
    "fetch('/api/tyfacet?t=' + typeof s"
    " + '&b=' + (s === true || s === false ? 1 : 0)"
    " + '&n=' + typeof +c"
    " + '&str=' + typeof ('a' + c));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
