/* THE OWED-SALE DOCUMENT (`owedsale`): a flow sold while the host still owes it an answer, vscodedev's shape.
   The page forks on absent application state. The assumed arm runs first and blocks on a synchronous host
   read the fixture holds (`held.` ops, answered by test_forced.c's hostreq_answer_all only once the other arm
   has asked `/api/osdue`), so its register is in the scheduler's owed set. The other arm grows the heap under
   a small RAM share (its TF_DOCS row), so pressure sells the blocked arm, and a sold register leaves the owed
   set, which is engine bookkeeping in no flow's world. The growing arm then reports its own arm.
   One of test_forced.c's exploring documents. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_OWEDSALE_H
#define ENGINE_HOST_FIXTURE_OWEDSALE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_OWEDSALE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var _oscfg = { hold: state.osHold };"
    "if (_oscfg.hold) {"
    "  var osV = hostRead('held.os');"
    "  fetch('/api/osdone?a=held&v=' + osV);"
    "} else {"
    "  var osGrow = [];"
    "  for (var i = 0; i < 800000; i++) osGrow.push({ i: i, s: 'v' + i });"
    "  fetch('/api/osdue?n=' + osGrow.length);"
    "  fetch('/api/osdone?a=free');"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
