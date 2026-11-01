/* THE ELEMENT STATE PSEUDO-CLASS DOCUMENT. HTML §4.16.3 "Pseudo-classes"' `:popover-open` (a popover whose
   visibility state is showing), `:modal` (a dialog whose is modal is true) and `:open` (a details or dialog with
   an open attribute; a select's drop-down box is never open here), read through `matches()` before and after
   showPopover(), showModal() and hidePopover(), with HTML §15.3.3's `[popover]:not(:popover-open)` rule read
   through getComputedStyle's `display`. Chrome 153 (Playwright) answers the thirteen values
   false,none,true,block,false,false,true,true,true,false,false,none,2 for this markup.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PSEUDOSTATE_H
#define ENGINE_HOST_FIXTURE_PSEUDOSTATE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PSEUDOSTATE[] =
    "<!doctype html><html><body>"
    "<div popover id=p>x</div><dialog id=d>m</dialog><details id=t open><summary>s</summary></details>"
    "<select id=s><option>a</select>"
    "<script>"
    "var p = document.getElementById('p'), d = document.getElementById('d'), r = [];"
    "r.push(p.matches(':popover-open'), getComputedStyle(p).display);"
    "p.showPopover();"
    "r.push(p.matches(':popover-open'), getComputedStyle(p).display);"
    "r.push(d.matches(':modal'), d.matches(':open'));"
    "d.showModal();"
    "r.push(d.matches(':modal'), d.matches(':open'));"
    "r.push(document.getElementById('t').matches(':open'), document.getElementById('s').matches(':open'));"
    "p.hidePopover();"
    "r.push(p.matches(':popover-open'), getComputedStyle(p).display, document.querySelectorAll(':open').length);"
    "fetch('/api/pseudostate?r=' + r.join('.'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
