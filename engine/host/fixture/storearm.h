/* THE STORED-STATE-ARM DOCUMENT: branches over application state the page's own origin stores. A key this run
   wrote reads back concrete (HTML §12.2.1 getItem). A localStorage key the area lacks and the cookies the jar
   lacks are unknowns whose example is that absence (core/storage/storage.c, core/dom/document_metadata.c), so an
   arm against the example takes absent application state as present and is priced as an assumption is
   (solver/flow.c's FLOW_PRICED_WORK, a floor under its code site's service). The example's world (`saOBS`) works
   a few quanta of its timer's code before its throw and the two priced worlds (`saLS`, `saCK`) throw at once, so
   without the price a fresh arm outranks the one whose code site has been served and throws first, as in
   HTML_ENVARM. One of test_forced.c's exploring documents (TF_DOCS); it ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STOREARM_H
#define ENGINE_HOST_FIXTURE_STOREARM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STOREARM[] =
    "<!doctype html><html><body>"
    "<script>"
    "localStorage.setItem('saSeen', 'saONE'); document.cookie = 'saCk=1';"
    "fetch('/api/saseen?ls=' + localStorage.getItem('saSeen'));"
    "var saW = localStorage.getItem('saTok') !== null ? 'saLS'"
    " : document.cookie.indexOf('saTok=') !== -1 ? 'saCK' : 'saOBS';"
    "fetch('/api/saarm?w=' + saW);"
    "setTimeout(function () {"
    " if (saW === 'saOBS') for (var saI = 0, saS = 0; saI < 50000; saI++) saS += saI;"
    " throw new Error('saBOOM' + saW); }, 0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
