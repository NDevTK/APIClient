/* THE FLAG-FLOW DOCUMENT: two click handlers linked only by two script-level bindings. Handler A arms a flag
   and stores the decoded fragment; handler B writes the stored value to innerHTML only while the flag is set.
   Nothing dispatches either click, so each is reached only as a drive, and the sink only in a world where B's
   drive follows A's. B's drive in a world without A is the control and reaches no sink. Each handler's
   witness fetch carries constants only.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLAGFLOW_H
#define ENGINE_HOST_FIXTURE_FLAGFLOW_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLAGFLOW[] =
    "<!doctype html><html><body>"
    "<button id='ffArm'>arm</button><button id='ffFire'>fire</button><div id='ffOut'></div>"
    "<script>"
    "var ffArmed = false, ffTarget = '';"
    "document.getElementById('ffArm').addEventListener('click', function ffArmClick() {"
    "  ffArmed = true;"
    "  ffTarget = decodeURIComponent(location.hash.slice(1));"
    "  fetch('/api/ffa-armed');"
    "});"
    "document.getElementById('ffFire').addEventListener('click', function ffFireClick() {"
    "  if (ffArmed) {"
    "    fetch('/api/ffb-armed');"
    "    document.getElementById('ffOut').innerHTML = ffTarget;"
    "  } else {"
    "    fetch('/api/ffb-unarmed');"
    "  }"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
