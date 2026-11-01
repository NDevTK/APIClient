/* THE ENVIRONMENT-ARM DOCUMENT: one branch over ECMAScript §21.3.2.28 Math.random ( ), an environment unknown with
   an example, and one over window.name, an attacker source. The Math.random branch's arm against its example is
   priced as an assumption is, so the example's world takes the first pull at code no member has served: each arm
   throws from a timer (HTML §8.7 Timers) with its own tag, and the order in which the stream announces the two
   throws is the order the two worlds reached them. The `envLO` arm works for under one pull (solver/flow.c's
   FLOW_PRICED_PULLS) of its timer's code before its throw and the other throws at once, so without the price the
   fresh arm outranks the one whose code site has been served and throws first; that `envLO` is the example's arm
   (the first Math.random() example of this document is below 0.5) is the row's premise, read off the grades. The
   window.name branch's arms cost nothing and are rank-equal at the fork, which solver/flow.c's flow_fork_inherit
   asserts over the whole weight. One of test_forced.c's exploring documents (TF_DOCS); it ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ENVARM_H
#define ENGINE_HOST_FIXTURE_ENVARM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ENVARM[] =
    "<!doctype html><html><body>"
    "<script>"
    "var envW = Math.random() < 0.5 ? 'envLO' : 'envHI';"
    "fetch('/api/envarm?w=' + envW);"
    "setTimeout(function () {"
    " if (envW === 'envLO') for (var envI = 0, envS = 0; envI < 3000; envI++) envS += envI;"
    " throw new Error('envBOOM' + envW); }, 0);"
    "var envN = window.name === 'envX' ? 'envNAMED' : 'envPLAIN';"
    "fetch('/api/envatk?w=' + envN);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
