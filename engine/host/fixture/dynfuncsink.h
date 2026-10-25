/* THE DYNAMIC-FUNCTION SINK DOCUMENT: §20.2.1.1.1 CreateDynamicFunction's source is a code-execution sink like
   §19.2.1.1 PerformEval's. The page hands `eval` the fragment and `new Function` the query, so each sink's record
   names its own source, plus one `new Function` over a constant body, which carries none. A Function sink whose
   source arrives as a plain string reports no source at all, which the second row reads.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DYNFUNCSINK_H
#define ENGINE_HOST_FIXTURE_DYNFUNCSINK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DYNFUNCSINK[] =
    "<!doctype html><html><body>"
    "<script>"
    "fetch('/api/dfs-start');"
    "try { eval(decodeURIComponent(location.hash.slice(1))); } catch (e) {}"
    "try { new Function('x', decodeURIComponent(location.search.slice(1))); } catch (e) {}"
    "new Function('x', 'return 1');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
