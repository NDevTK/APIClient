/* THE TIMERS DOCUMENT: HTML §8.7 "Timers" and its ordering against microtasks, one program.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TIMERS_H
#define ENGINE_HOST_FIXTURE_TIMERS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TIMERS[] =
    "<!doctype html><html><body><script>"
    /* HTML §8.7 Timers's TASK SOURCE, and §8.1.7's ordering around it — neither of which this fixture exercised
       at all, so `setTimeout` had no probe in the engine's own test despite being how a great deal of real code
       reaches the event loop. The ORDER is the assertion: a microtask checkpoint runs before the next task, so
       the `.then` continuation must observe its marker BEFORE the timer callback runs, and two timers with the
       same delay run in the order they were set. */
    "var _tk = '';"
    "setTimeout(function(){ _tk += 'B'; }, 0);"
    "setTimeout(function(){ _tk += 'C'; fetch('/api/timerfire?v=' + _tk); }, 0);"
    "Promise.resolve().then(function(){ _tk += 'A'; });"

    /* §8.7's STRING HANDLER, AND STEP 2's IDENTIFIER ACROSS THE TWO SPELLINGS OF THE HANDLER UNION. The three
       rows above all pass a Function, so the arm of the timer initialization steps whose handler is compiled
       as a classic script ran in no test in this tree. `strran` is that program having actually run; `next` is
       the two spellings drawing from ONE per-global counter, which is what makes a handle name at most one
       timer. A second counter, or a path that forgets to bump, gives two live timers one identifier and
       `clearTimeout` then clears the wrong one — a defect with no other symptom, since both calls still return
       a plausible number. (That the string form makes a map ENTRY at all is what the two rows below are for:
       it did not, and `next` was true anyway, because a minted-and-discarded handle counts the same.) */
    "var _tsh1 = setTimeout(\"fetch('/api/timerstr?v=strran');\", 0);"
    "var _tsh2 = setTimeout(function(){ fetch('/api/timerhandle?h=' +"
    " (_tsh2 === _tsh1 + 1 ? 'next' : 'reused')); }, 0);"

    /* §8.7's STRING HANDLER IS A TIMER, WHICH IS THE HALF THE ROW ABOVE CANNOT SEE: `strran` says the program
       RAN and nothing about WHEN. A timer is a `timeout` that orders it and a map entry `clearTimeout` removes,
       and neither has any other symptom when missing.
       `cleared` IS THE CANCELLATION: a string-handled timer cleared before it fires leaves no record, so
       `notcleared` appearing is `clearTimeout` looking up an identifier that names nothing.
       `ba` IS THE `timeout`: §8.7 orders the timer task source by expiry, so the 0-delay handler runs before
       the 100-delay one and the later handler reads both letters; queued at the set, both run in SOURCE order.
       The spec's own worked example (`setTimeout({toString(){ setTimeout("logger('ONE')", 100); return
       "logger('TWO')" }}, 100)`) logs "ONE TWO" under either placement, so it is not the discriminator.
       IT NEEDS THE VIRTUAL CLOCK TO MOVE FORWARD — every other timer here is 0-delay — so a 0 on `timerdelay`
       is two claims at once (core/timing/event_loop.c's event_loop_may_advance among them), and its
       NOT-REACHED text says so. */
    "var _tso = '';"
    "var _tsc = setTimeout(\"fetch('/api/timerclr?v=notcleared');\", 0);"
    "clearTimeout(_tsc);"
    "setTimeout(\"_tso += 'a'; fetch('/api/timerdelay?v=' + _tso);\", 100);"
    "setTimeout(\"_tso += 'b'; fetch('/api/timerclr?v=cleared');\", 0);"

    /* §8.7 STEP 4 OVER AN UNKNOWN `timeout` — "if timeout is less than 0, then set timeout to 0" asked about
       a value nothing computed, which is a question with two real answers and therefore a FORK.
       IT IS THE FORK AND NOT THE TIMER THAT IS PROBED, and the `clearTimeout` is what keeps it that way. The
       arm that keeps the unknown writes an expiry no double can express, and FIRING one moves the event
       loop's virtual clock to a moment no arm has decided — a capability core/timing/event_loop.c does not
       have and whose absence timer_run_due names in full. Cleared, both arms reach the same end and what the
       row asserts is the thing that was missing: that a fork asked from inside §8.7's own algorithm has
       somewhere to put its sibling. As a plain JSCFunction it had none — the seam crashed at the fork, naming
       the predicate, so this ONE LINE took the whole document down before any of it ran. `unkdelay` is
       therefore a row that reads 0 when the members stop being step machines, and it reads 0 by ABORTING. */
    "var _t4 = setTimeout(function(){ fetch('/api/unkdelay/fired'); }, state.d);"
    "clearTimeout(_t4);"
    "fetch('/api/unkdelay?v=' + (typeof _t4 === 'number' ? 'forked' : 'nohandle'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
