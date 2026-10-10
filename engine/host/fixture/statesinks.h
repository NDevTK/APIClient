/* THE INJECTED-STATE SINKS DOCUMENT: server-injected `state` members into a script, a comment, markup and a navigation.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STATESINKS_H
#define ENGINE_HOST_FIXTURE_STATESINKS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STATESINKS[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "eval(\"'\" + state.code + \"'\");"   /* @S JS: source lands INSIDE a §12.9.4 SingleStringCharacters state -> solve_js.c derives that state's own exit, `';X9()//` */
    /* @S JS, A SECOND §12 STATE, and the one that says the derivation is a derivation. The hole lands in a
       §12.4 SingleLineComment, which NONE of the five deleted CANDS_JS payloads could fit: every one of them
       continued the comment it was already inside, so this sink was unsolvable BY CONSTRUCTION and reported
       `parked, tried 5` without ever naming what it had failed to escape. solve_js.c scans the argument the
       probe run produced, names the state, and emits that state's own exit — §12.4 puts the LineTerminator
       OUTSIDE the comment and §12.10 rule 1 inserts the semicolon across it, so the whole escape is one
       newline and the call. */
    "eval('//' + state.note);"
    "setInnerHTML('<div>' + state.html + '</div>');"   /* @S HTML: source in HTML text -> breakout an auto-firing element */
    "setLocation(state.next);"   /* @S URL: attacker controls the whole URL -> breakout javascript:X9() */
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
