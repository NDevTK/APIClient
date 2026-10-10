/* THE MESSAGING DOCUMENT: postMessage and BroadcastChannel deliveries carrying an unknown.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MESSAGING_H
#define ENGINE_HOST_FIXTURE_MESSAGING_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MESSAGING[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* A CONCOLIC ACROSS A TURN BOUNDARY, WHICH IS THE ONE DELIVERY §9.3.3 MAKES AND THIS FIXTURE HAD NO
       STATEMENT FOR. `postMessage` serializes NOW (step 7) and delivers in a LATER TASK (step 8), so the value
       does not cross as a live JSValue at all — §Security says one crosses neither a park, a session nor an
       instance — and what the receiving handler is handed is whatever the record carried. THAT MAKES IT THE
       CHEAPEST TEST THERE IS OF WHETHER AN ATTACKER SOURCE SURVIVES A QUEUE: §Attacker-sources rates
       `message.data` one of the platform's richest, and a page posting `location.hash` is the canonical shape.
       THE TWO TOKENS ARE THE WHOLE ASSERTION AND THEY ARE CONSTANTS. If the delivered value is still the
       triple, the `===` is a branch over an unknown and §Solver-half forks it, so BOTH `scADMIN` and
       `scPUBLIC` are in the document. If the record carried the EXAMPLE alone, the comparison is DECIDED and
       exactly one of them is — which is a wrong answer and not a lossy one, because the admin arm is the
       surface this tool exists to reach. Neither token is composed from anything this engine computed, for
       §A-WITNESS-CARRIES-CONSTANTS-ONLY's reason: a payload built from a concolic can itself be unknown, and
       then the request is never made and a reached arm reads exactly like an arm that did not run.
       `scENTER` IS SEPARATE AND IS THE REACHABILITY HALF, because an absent fork token has two readings — the
       delivery never ran, or it ran and the branch was decided — and those take opposite work. */
    "window.addEventListener('message', function(e){ fetch('/api/scmsg?v=scENTER');"
    " if (e.data === 'adminpanel') { fetch('/api/scfork?w=scADMIN'); } else { fetch('/api/scfork?w=scPUBLIC'); } });"
    "window.postMessage(location.hash.slice(1), '*');"
    /* AND THE SAME ASSERTION OVER §9.5's BUS, WHICH IS A DIFFERENT CARRIER AND THEREFORE A DIFFERENT CLAIM.
       §9.3.3's post rides StructuredSerializeWithTransfer, so its record always had a second array to carry a
       triple; §9.5's rode §2.7.4, whose out-parameter is BYTES AND NOTHING BESIDE THEM, and a concolic reached
       from inside the message was refused BY NAME — so `postMessage(location.hash)` on a channel ABORTED THE
       INSTANCE where a real browser delivers a string, and every row of this document went dark with it. The
       two rows cannot be folded into the window ones for that reason: one record carried a triple and the
       other could not, and a single pair would report both as whichever happened to be broken.
       TWO CHANNELS AND NOT ONE, because §9.5's postMessage steps exclude the SENDER — a page that posts on its
       own channel does not hear itself, which is the first thing every user of the bus relies on. So the
       receiver is opened first and the post is made on the second.
       THE TOKENS ARE CONSTANTS for §A-WITNESS-CARRIES-CONSTANTS-ONLY's reason, and `bcENTER` is separate from
       the fork pair for the reason `scENTER` is: an absent fork token has two readings — the delivery task
       never ran, or it ran and the branch was DECIDED — and those take opposite work. */
    "var _bcrecv = new BroadcastChannel('tf-bus');"
    "var _bcsend = new BroadcastChannel('tf-bus');"
    "_bcrecv.addEventListener('message', function(e){ fetch('/api/bcmsg?v=bcENTER');"
    " if (e.data === 'adminpanel') { fetch('/api/bcfork?w=bcADMIN'); } else { fetch('/api/bcfork?w=bcPUBLIC'); } });"
    "_bcsend.postMessage(location.hash.slice(1));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
