/* THE TIME-GATE DOCUMENT: `Math.random()` and the clock each gate two branches with an address of their own, the
   shapes of a sampled-telemetry gate and a time-of-day banner. No served byte states either value, so each is
   an unknown whose every arm runs; one sample from the host decides the branch and deletes the other arm. Each
   address is a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TIMEGATE_H
#define ENGINE_HOST_FIXTURE_TIMEGATE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TIMEGATE[] =
    "<!doctype html><html><body>"
    "<script>"
    "if (Math.random() < 0.01) fetch('/api/tg-sampled');"
    "else fetch('/api/tg-unsampled');"
    "</script>"
    "<script>"
    "if (new Date(Date.now()).getUTCHours() >= 22) fetch('/api/tg-night');"
    "else fetch('/api/tg-day');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
