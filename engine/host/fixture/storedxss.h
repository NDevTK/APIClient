/* THE STORED-XSS DOCUMENT: values a previous load stored are read on load and written to innerHTML. An attacker
   sets only the URL, window.name, a message from a window it can reach, and the victim's interaction, so a
   stored value is attacker input only where this page itself stored it from one of those.
   - A bare read of localStorage's `sxNote` or of the cookie `sxBanner` into markup is a precondition, not an
     attacker PoC: the sink is reached, and no fired PoC may claim a delivery for it.
   - The page's own write paths make them PoCs: a load carrying a fragment saves it as localStorage's
     `sxDraft`, and a load carrying `?theme=` saves it as the cookie `sxTheme`; a later load shows each. The
     PoC is two ordered acts: visit with the address, then the attacker opens the address again in a new
     window, whose load reads the user agent's storage the first one wrote.
   `storedread` is the negative of the act: a page that only reads a key it never wrote stays a precondition
   finding, and the open is never offered, since without a write a second load is the first one again.
   `storedidb` is the same write path through IndexedDB: a load carrying a fragment puts it in the `memos`
   store, and a later load reads it back into markup.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STOREDXSS_H
#define ENGINE_HOST_FIXTURE_STOREDXSS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STOREDXSS[] =
    "<!doctype html><html><body>"
    "<div id='sxLs'></div><div id='sxDraftView'></div><div id='sxCk'></div><div id='sxThemeView'></div>"
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
    "function sxCookie(name) {"
    "  var jar = document.cookie, at = jar.indexOf(name + '=');"
    "  return at < 0 ? null : decodeURIComponent(jar.slice(at + name.length + 1).split(';')[0]);"
    "}"
    "var sxBanner = sxCookie('sxBanner');"
    "if (sxBanner !== null) {"
    "  fetch('/api/sx-ck-read');"
    "  document.getElementById('sxCk').innerHTML = sxBanner;"
    "}"
    "</script>"
    "<script>"
    "var sxTheme = sxCookie('sxTheme');"
    "if (sxTheme !== null) {"
    "  fetch('/api/sx-theme-shown');"
    "  document.getElementById('sxThemeView').innerHTML = sxTheme;"
    "} else {"
    "  var sxAt = location.search.indexOf('theme=');"
    "  if (sxAt >= 0) {"
    "    var sxAsk = decodeURIComponent(location.search.slice(sxAt + 6).split('&')[0]);"
    "    document.cookie = 'sxTheme=' + encodeURIComponent(sxAsk) + '; path=/';"
    "    fetch('/api/sx-theme-saved');"
    "  }"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_STOREDREAD[] =
    "<!doctype html><html><body>"
    "<div id='rdView'></div>"
    "<script>"
    "var rdNote = localStorage.getItem('rdNote');"
    "if (rdNote !== null) {"
    "  fetch('/api/rd-read');"
    "  document.getElementById('rdView').innerHTML = rdNote;"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_STOREDIDB[] =
    "<!doctype html><html><body>"
    "<div id='siView'></div>"
    "<script>"
    "var siReq = indexedDB.open('siDb', 1);"
    "siReq.onupgradeneeded = function () { siReq.result.createObjectStore('memos'); };"
    "siReq.onsuccess = function () {"
    "  var db = siReq.result, got = db.transaction('memos').objectStore('memos').get('memo');"
    "  got.onsuccess = function () {"
    "    if (got.result !== undefined) {"
    "      fetch('/api/si-memo-shown');"
    "      document.getElementById('siView').innerHTML = got.result;"
    "    } else if (location.hash.length > 1) {"
    "      var tx = db.transaction('memos', 'readwrite');"
    "      tx.objectStore('memos').put(decodeURIComponent(location.hash.slice(1)), 'memo');"
    "      tx.oncomplete = function () { fetch('/api/si-memo-saved'); };"
    "    }"
    "  };"
    "};"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
