/* The waiting-clock document. One of test_forced.c's exploring documents (its TF_DOCS table). The zone refuses
   the page's POST to /api/decl (HTML_DECLINES's rule), so the refusal forks a waiting arm that keeps the
   unanswered record. In that arm a timer's deadline and an animation frame stand ahead of the clock while
   nothing is in flight: HTML §8.7 Timers waits on the document's time and HTML §8.1.7.3 Processing model
   determines a rendering opportunity from the display and the page, so both run as they do in a browser whose
   request never settles, and neither is raced against an arrival. A request in flight beside the refusal is
   then raced against a timer in both orders. Every reported value carries the POST's state as the page saw it.
   It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WAITCLOCK_H
#define ENGINE_HOST_FIXTURE_WAITCLOCK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WAITCLOCK[] =
    "<!doctype html><html><body>"
    "<script>"
    /* The outer timer starts the statement once nothing else is due, so the document's first frame is not
       standing ahead of the clock while the requests below are in flight. */
    "setTimeout(function () {"
    " var wcPost = 'wcWaiting';"
    " fetch('/api/decl/wclk', { method: 'POST' })"
    "  .then(function () { wcPost = 'wcAnswered'; }, function () { wcPost = 'wcFailed'; });"
    /* Two served requests in turn. The first shares the POST's payment, and by the second's reaction the
       refusal's failure and success arms have delivered their outcome, so only the waiting arm goes on. It has
       taken two replies there, and nothing is in flight. */
    " fetch('/api/wclktick1').then(function () {"
    "  fetch('/api/wclktick2').then(function () {"
    "   var seen = [];"
    "   if (wcPost !== 'wcWaiting') { fetch('/api/wclksettled?v=' + wcPost); return; }"
    /* The request in flight beside the refusal, issued from the third reply's reaction. */
    "   var mixed = function () {"
    "    var order = [];"
    "    var done = function (t) {"
    "     order.push(t);"
    "     if (order.length === 2) fetch('/api/wclkmixed?v=' + order.join('-') + '-' + wcPost);"
    "    };"
    "    fetch('/api/wclkreply').then(function () { done('reply'); });"
    "    setTimeout(function () { done('timer'); }, 50);"
    "   };"
    "   var ran = function (t) {"
    "    seen.push(t);"
    "    if (seen.length === 2) fetch('/api/wclkwaiting?v=' + seen.join('-') + '-' + wcPost).then(mixed);"
    "   };"
    /* The first frame is due at once (the outer timer left the clock past it); the frame it requests is one
       refresh interval ahead of the clock, as the timer's deadline is. */
    "   setTimeout(function () { ran('timer'); }, 40);"
    "   requestAnimationFrame(function () { requestAnimationFrame(function () { ran('frame'); }); });"
    "  });"
    " });"
    "}, 9000);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
