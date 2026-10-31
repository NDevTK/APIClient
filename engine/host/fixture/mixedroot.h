/* THE MIXED-ROOT DOCUMENT: one value whose first operand is an environment unknown (Math.random) reaches the
   markup sink twice, once alone and once joined with document.cookie, which declares application state. Both
   writes carry the same injection identity (a derivation takes its operand's), and only the second has a
   member another party sets. Each write is followed by a request, the reachability witness its row reads.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MIXEDROOT_H
#define ENGINE_HOST_FIXTURE_MIXEDROOT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MIXEDROOT[] =
    "<!doctype html><html><body>"
    "<div id='mrA'></div><div id='mrB'></div>"
    "<script>"
    "var mrTag = String(Math.random());"
    "document.getElementById('mrA').innerHTML = mrTag;"
    "fetch('/api/mr-alone');"
    "document.getElementById('mrB').innerHTML = mrTag + document.cookie;"
    "fetch('/api/mr-joined');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
