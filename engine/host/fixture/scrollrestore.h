/* THE SCROLL RESTORATION DOCUMENTS: HTML §7.4.6.5 "Persisted history entry state" over a same-document
   traversal. One of test_forced.c's exploring documents each (its TF_DOCS table).
   `scrollrestore` scrolls the viewport to 400, pushes an entry, scrolls to 900 and goes back. The text saves
   persisted state only in §7.4.6.1's activate history entry, to the outgoing active entry: going back saves 900
   into the pushed entry, and the first entry, left by `pushState` rather than by an activation, holds no scroll
   position data, so nothing is restored and the row reads r1=900. Chrome 155.0.8059.84 reads 400 after
   `popstate`, a save at push time the text has no step for (docs/claude-record.md).
   `scrollclamp` saves 1500, then shrinks the document to fit the viewport before going back: the restored 1500
   lies outside the document's scrolling area now, and Chrome 155.0.8059.84 lands at 0 (r2=0).
   Under the text neither of those two restores anything: each goes back to an entry no activation saved.
   `scrollfwd` restores. It goes back (saving 900 into the pushed entry), scrolls to 200 in that popstate and goes
   forward: forward saves 200 into the first entry and, after the second popstate (p=200), restores the pushed
   entry's 900 (r3=900). `scrollfwdclamp` is the same with the document shrunk to 100px inside the second popstate,
   so the restore, measured after it, clamps 900 into an empty scrolling area (r4=0) while p is still 200.
   Each ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
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

static const char HTML_SCROLLFWD[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\"><div style=\"height:3000px\"></div>"
    "<script>"
    "scrollTo(0,400);history.pushState({a:1},'','#b');scrollTo(0,900);var n=0;"
    "addEventListener('popstate',function(){n++;if(n==1){scrollTo(0,200);history.forward();return;}var p=scrollY;"
    "setTimeout(function(){fetch('/api/scrollfwd?p='+p+'&r3='+scrollY);},200);});"
    "history.back();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_SCROLLFWDCLAMP[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\"><div id=t style=\"height:3000px\"></div>"
    "<script>"
    "scrollTo(0,400);history.pushState({a:1},'','#b');scrollTo(0,900);var n=0;"
    "addEventListener('popstate',function(){n++;if(n==1){scrollTo(0,200);history.forward();return;}var p=scrollY;"
    "document.getElementById('t').style.height='100px';"
    "setTimeout(function(){fetch('/api/scrollfwdclamp?p='+p+'&r4='+scrollY);},200);});"
    "history.back();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
