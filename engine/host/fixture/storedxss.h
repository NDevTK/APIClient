/* THE STORED-XSS DOCUMENT: values a previous load stored are read on load and written to innerHTML.
   - localStorage is a same-origin store with no cross-origin write path, so a bare read of `sxNote` into
     markup is a same-origin precondition and not an attacker PoC: the sink is reached, and no fired PoC may
     claim a delivery for it.
   - The page's own write path makes it one: a load carrying a fragment saves it as `sxDraft`, and a later load
     shows `sxDraft`. The attacker source is the fragment, so the PoC is two ordered acts: visit with the
     fragment, then load the page again.
   - A cookie can be set by a sibling subdomain, so the cookie arm is an attacker PoC whose delivery is planted
     and states that precondition.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STOREDXSS_H
#define ENGINE_HOST_FIXTURE_STOREDXSS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STOREDXSS[] =
    "<!doctype html><html><body>"
    "<div id='sxLs'></div><div id='sxDraftView'></div><div id='sxCk'></div>"
    "<script>"
    "var sxNote = localStorage.getItem('sxNote');"
    "if (sxNote !== null) {"
    "  fetch('/api/sx-ls-read');"
    "  document.getElementById('sxLs').innerHTML = sxNote;"
    "}"
    "</script>"
    "<script>"
    "var sxDraft = localStorage.getItem('sxDraft');"
    "if (sxDraft !== null) {"
    "  fetch('/api/sx-draft-shown');"
    "  document.getElementById('sxDraftView').innerHTML = sxDraft;"
    "} else if (location.hash.length > 1) {"
    "  localStorage.setItem('sxDraft', decodeURIComponent(location.hash.slice(1)));"
    "  fetch('/api/sx-draft-saved');"
    "}"
    "</script>"
    "<script>"
    "var sxJar = document.cookie;"
    "var sxAt = sxJar.indexOf('sxBanner=');"
    "if (sxAt >= 0) {"
    "  fetch('/api/sx-ck-read');"
    "  document.getElementById('sxCk').innerHTML = decodeURIComponent(sxJar.slice(sxAt + 9).split(';')[0]);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
