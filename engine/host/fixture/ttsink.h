/* THE TRUSTED-TYPES SINK DOCUMENT: the page's policy requires Trusted Types for script sinks and allows one
   policy, `appPolicy`, whose createHTML escapes `<`. The query reaches innerHTML through that policy, so its
   search must not fire; the fragment reaches innerHTML as a plain string with no default policy, so Trusted
   Types §3.4 Get Trusted Type compliant string throws a TypeError at the write and no breakout is ever
   parsed. Both sinks are real; neither is an XSS on this page, and the policy is what says so.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TTSINK_H
#define ENGINE_HOST_FIXTURE_TTSINK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TTSINK[] =
    "<!doctype html><html><head>"
    "<meta http-equiv='Content-Security-Policy' content=\"require-trusted-types-for 'script'; trusted-types appPolicy\">"
    "</head><body>"
    "<div id='ttSafe'></div><div id='ttRaw'></div>"
    "<script>"
    "var ttPolicy = trustedTypes.createPolicy('appPolicy', {"
    "  createHTML: function (s) { return s.split('<').join('&lt;'); }"
    "});"
    "document.getElementById('ttSafe').innerHTML ="
    "  ttPolicy.createHTML(decodeURIComponent(location.search.slice(1)));"
    "</script>"
    "<script>"
    "try {"
    "  document.getElementById('ttRaw').innerHTML = decodeURIComponent(location.hash.slice(1));"
    "  fetch('/api/tt-raw-wrote');"
    "} catch (e) {"
    "  fetch('/api/tt-raw-blocked?e=' + e.name);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
