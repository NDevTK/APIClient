/* THE FRAGMENT SCRIPT DOCUMENT: the two ways markup written through a fragment member runs the page's code, both
   inside the member and both after DOM §4.2.3 insert's per-node loop.
   1. innerHTML into a connected `script` element that has not started: insert runs the children changed steps
      once the Text child is in place, and HTML §4.12.1.1's script children changed steps execute the element.
      The program records how many children the script element had when it ran (1).
   2. setHTMLUnsafe with runScripts into a connected element: §13.4's Fragment scripting mode leaves the parsed
      `script` unstarted, and insert runs its post-connection steps over staticNodeList only after every node of
      the fragment is inserted, so the script sees all three of its target's children (3).
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAGSCRIPT_H
#define ENGINE_HOST_FIXTURE_FRAGSCRIPT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAGSCRIPT[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var fss = document.createElement('script');"
    "document.body.appendChild(fss);"
    "fss.innerHTML = \"fetch('/api/fragscript?v=' + document.body.lastChild.childNodes.length)\";"
    "var fsu = document.createElement('div');"
    "fsu.id = 'fsu';"
    "document.body.appendChild(fsu);"
    "fsu.setHTMLUnsafe(\"<script>fetch('/api/fragscriptu?w=' + document.getElementById('fsu').childNodes.length)"
    "</scr\" + \"ipt><i></i><i></i>\", { runScripts: true });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
