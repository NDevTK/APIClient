/* THE PER-TIMELINE FRAME REALM DOCUMENT: a srcless <iframe> whose initial about:blank Document each timeline that
   reads through it materializes in a realm of its own, so one document handle names two realms. The script forks on
   the fragment before either arm reads the frame. One arm reads the frame and removes it, so once the removal's
   queued destruction has run its realm for the document is reachable from nothing. The other arm waits three tasks,
   reads the frame and writes two scripts into its document with document.write: an external one, which blocks the
   parser while its bytes are fetched, and an inline one behind it, a program row of the document that cannot start
   before the first has run. In that same task it calls the fixture's `gcNow()`, which collects the first arm's realm
   while this arm's rows, minted for its own live realm, are unstarted. The witness fetches carry constants, and the
   frame's scripts fetch through `parent`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_NAVREALM_H
#define ENGINE_HOST_FIXTURE_NAVREALM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_NAVREALM[] =
    "<!doctype html><html><body>"
    "<iframe id='nrf'></iframe>"
    "<script>"
    "function nrArm() {"
    "  var f = document.getElementById('nrf');"
    "  if (location.hash === '#nr') {"
    "    setTimeout(function nrWait1() { setTimeout(function nrWait2() { setTimeout(function nrQueue() {"
    "      var d = f.contentDocument;"
    "      d.open();"
    "      d.write(\"<script src='https://x.test/chunk/nrframe.js'><\\/script>\");"
    "      d.write(\"<script>parent.fetch('/api/nr-inline-ran')<\\/script>\");"
    "      d.close();"
    "      fetch('/api/nr-queued');"
    "      gcNow();"
    "    }, 0); }, 0); }, 0);"
    "  } else {"
    "    (function () { var d = f.contentDocument; d.body.appendChild(d.createElement('p')); })();"
    "    f.remove();"
    "    fetch('/api/nr-removed');"
    "  }"
    "}"
    "nrArm();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
