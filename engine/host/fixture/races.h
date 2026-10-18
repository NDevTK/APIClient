/* THE RACES DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table). A timer's deadline that
   the clock has not reached, beside replies in flight, is raced against each reply's unknown arrival
   (core/timing/event_loop.h's event_loop_deadline_first and event_loop_deadline_before_arrival), so every
   order of the deadline among the arrivals runs. These statements stand alone so that the timers of other
   statements do not multiply their orders: with m timers ahead of N arrivals the orders are C(N+m, m). It
   ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RACES_H
#define ENGINE_HOST_FIXTURE_RACES_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RACES[] =
    "<!doctype html><html>"
    "<body>"
    /* A timer's deadline ahead of the clock while a reply is in flight is raced against the reply's unknown
       arrival (HTML §8.1.7.3 step 2.1; §8.7's run steps after a timeout), so both orders run: `reply-timer`
       and `timer-reply`. The outer timer runs the pair once nothing else is due, so the flow is idle with the
       reply owed. */
    "<script>"
    "setTimeout(function () {"
    " var seen = [];"
    " function done(t) { seen.push(t); if (seen.length === 2) fetch('/api/xraceorder?v=' + seen.join('-')); }"
    " fetch('/api/xrace').then(function () { done('reply'); });"
    " setTimeout(function () { done('timer'); }, 50);"
    "}, 9000);"
    "</script>"
    /* The same race over a chain: the second request is issued from the first reply's reaction, so each
       arrival is its own unknown and the 50 ms deadline is raced at each, three orders: `timer-a-b`,
       `a-timer-b` and `a-b-timer`. */
    "<script>"
    "setTimeout(function () {"
    " var seen = [];"
    " function done(t) { seen.push(t); if (seen.length === 3) fetch('/api/xchainorder?v=' + seen.join('-')); }"
    " fetch('/api/xchaina').then(function () { done('a');"
    "  fetch('/api/xchainb').then(function () { done('b'); }); });"
    " setTimeout(function () { done('timer'); }, 50);"
    "}, 9500);"
    "</script>"
    /* Two requests issued together, which the host answers in one payment: the delivery arm races the 50 ms
       deadline before each arrival, so the timer can run between the two reactions too: `timer-a-b`,
       `a-timer-b` and `a-b-timer`. */
    "<script>"
    "setTimeout(function () {"
    " var seen = [];"
    " function done(t) { seen.push(t); if (seen.length === 3) fetch('/api/xpairorder?v=' + seen.join('-')); }"
    " fetch('/api/xpaira').then(function () { done('a'); });"
    " fetch('/api/xpairb').then(function () { done('b'); });"
    " setTimeout(function () { done('timer'); }, 50);"
    "}, 10000);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
