/* THE MESSAGE STATE-MACHINE DOCUMENT UNDER COOP: fixture/msgchain.h's page, served with Cross-Origin-Opener-
   Policy `same-origin` (test_forced.c's TF_DOCS row states it), with its own emission names so each row
   belongs to one document. An attacker that opened this page holds no opener reference to it, so a message act
   reaching its handler states no opener route and no PoC names one.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGCHAINCOOP_H
#define ENGINE_HOST_FIXTURE_MSGCHAINCOOP_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGCHAINCOOP[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* The emissions are constants, so a reached arm is told apart from an arm that never ran. */
    "var mccEl = document.createElement('div'); document.body.appendChild(mccEl);"
    "if (new URLSearchParams(location.search).get('mode') === 'dev') {"
    " fetch('/api/mccgate?v=mccLISTEN');"
    " var mccState = 0;"
    " window.addEventListener('message', function (e) {"
    "  if (e.origin.indexOf('trusted.example') === -1) return;"
    "  var d = e.data;"
    "  if (!d || typeof d !== 'object') return;"
    "  if (mccState === 0) {"
    "   if (d.type === 'init' && typeof d.session === 'string') { mccState = 1; fetch('/api/mccstep?v=mccINIT'); }"
    "   return;"
    "  }"
    "  if (mccState === 1) {"
    "   if (d.type === 'cfg' && d.version === 2 && typeof d.theme === 'string') {"
    "    mccState = 2; fetch('/api/mccstep?v=mccCFG');"
    "   }"
    "   return;"
    "  }"
    "  if (mccState === 2 && d.type === 'go' && typeof d.html === 'string') {"
    "   fetch('/api/mccstep?v=mccGO'); mccEl.innerHTML = d.html;"
    "  }"
    " });"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
