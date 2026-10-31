/* THE SCROLL RESTORATION DOCUMENTS: HTML §7.4.6.5 "Persisted history entry state" over a same-document
   traversal. One of test_forced.c's exploring documents each (its TF_DOCS table).
   `scrollrestore` scrolls the viewport to 400, pushes an entry, scrolls to 900 and goes back: save persisted
   state records 900 for the pushed entry, and restore persisted state puts the first entry's 400 back, which
   Chrome 155.0.8059.84 reads after `popstate` (r1=400; scrollY at `popstate` itself is still 900).
   `scrollclamp` saves 1500, then shrinks the document to fit the viewport before going back: the restored 1500
   lies outside the document's scrolling area now, and Chrome 155.0.8059.84 lands at 0 (r2=0).
   This engine holds no scroll position data yet, and core/frame/session_history.c asserts, wherever it saves or
   restores persisted state, that the viewport is at its origin: `scrollrestore` is at 900 there, so the assert
   fires until the data is built; `scrollclamp` is back at 0 before it goes back, so the assert holds, which is
   its negative control. Each ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SCROLLRESTORE_H
#define ENGINE_HOST_FIXTURE_SCROLLRESTORE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SCROLLRESTORE[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\"><div style=\"height:3000px\"></div>"
    "<script>"
    "scrollTo(0,400);history.pushState({a:1},'','#b');scrollTo(0,900);"
    "addEventListener('popstate',function(){var p=scrollY;"
    "setTimeout(function(){fetch('/api/scrollrestore?p='+p+'&r1='+scrollY);},200);});"
    "history.back();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_SCROLLCLAMP[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\"><div id=t style=\"height:3000px\"></div>"
    "<script>"
    "scrollTo(0,1500);history.pushState({a:1},'','#b');scrollTo(0,0);"
    "document.getElementById('t').style.height='100px';"
    "addEventListener('popstate',function(){"
    "setTimeout(function(){fetch('/api/scrollclamp?r2='+scrollY);},200);});"
    "history.back();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
