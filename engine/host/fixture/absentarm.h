/* THE ABSENT-ARM DOCUMENT: one branch over a global this session does not have. A property read that misses the
   global object answers undefined in this session (ECMAScript §10.1.8.1 OrdinaryGet step 2.b), and the engine
   mints an unknown for it (solver/absent.c), so the branch forks: the falsy arm is the one this session takes,
   and the truthy arm is one the engine chose. Each arm makes one request and then throws the same uncaught error
   with its own tag, so the probe rows can read which arm's records are graded forced and which arm's page error
   is filed as this engine's exploration.
   A first script spends an unknown read off the global as a property key twice, once as `k + ""` and once as
   `k`: §7.1.21 ToPropertyKey makes them one name, so the value written under one is read under the other,
   while `k + "5"` is a different name and does not read that slot.
   The same script adds another unknown to the Number 0 and to the String "0" and sends both in one address, so
   the probe rows can read that a sum and a concatenation spell two shapes, and that a concatenation onto a
   String the page already built takes the string arm.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ABSENTARM_H
#define ENGINE_HOST_FIXTURE_ABSENTARM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ABSENTARM[] =
    "<!doctype html><html><body>"
    "<script>"
    "var absKo = {}; var absK = window.__tfAbsentKey; absKo[absK + ''] = 'str';"
    "fetch('/api/abskey?same=' + (absKo[absK] === 'str') + '&other=' + (absKo[absK + '5'] === undefined));"
    "var absS = window.__tfAbsSum; fetch('/api/abssum?n=' + (0 + absS) + '&t=' + ('0' + absS));"
    "</script>"
    "<script>"
    "function absBoom(tag) { throw new Error('absBOOM' + tag); }"
    "if (window.__tfAbsentGate) { fetch('/api/abschosen'); absBoom('ARM'); }"
    " else { fetch('/api/absobserved'); absBoom('OBS'); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
