/* THE ABSENT-ARM DOCUMENT: one branch over a global this session does not have. A property read that misses the
   global object answers undefined in this session (ECMAScript §10.1.8.1 OrdinaryGet step 2.b), and the engine
   mints an unknown for it (solver/absent.c), so the branch forks: the falsy arm is the one this session takes,
   and the truthy arm is one the engine chose. Each arm makes one request and then throws the same uncaught error
   with its own tag, so the probe rows can read which arm's records are graded forced and which arm's page error
   is filed as this engine's exploration.
   A first script writes under an unknown read off the global as `k + ""` and reads back under `k`, `String(k)`
   and `${k}`: §7.1.21 ToPropertyKey makes all four one name, so each read sees the write, while `k + "5"` is a
   different name and does not read that slot.
   It also adds the Number 0 and the String "0" to another unknown `s`, which holds `undefined`, and sends both
   twice: alone, so the address carries §13.15.3's computed bytes, and beside `Math[s]`, a key read with no
   example, so that address spells shapes. A sum and a concatenation take two arms and two shapes.
   Over two more unknowns, `undefined` here, `m?.deep` short-circuits and `Object.prototype.toString.call(t)`
   answers (§20.1.3.6) on the session's own arm. One of test_forced.c's exploring documents (TF_DOCS); it ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ABSENTARM_H
#define ENGINE_HOST_FIXTURE_ABSENTARM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ABSENTARM[] =
    "<!doctype html><html><body>"
    "<script>"
    "var absKo = {}; var absK = window.__tfAbsentKey; absKo[absK + ''] = 'str';"
    "fetch('/api/abskey?same=' + (absKo[absK] === 'str') + '&str=' + (absKo[String(absK)] === 'str') +"
    " '&tpl=' + (absKo[`${absK}`] === 'str') + '&other=' + (absKo[absK + '5'] === undefined));"
    "var absS = window.__tfAbsSum; fetch('/api/abssum?n=' + (0 + absS) + '&t=' + ('0' + absS));"
    "fetch('/api/abssumx?n=' + (0 + absS) + '&t=' + ('0' + absS) + '&k=' + Math[absS]);"
    "var absM = window.__tfMaybe; fetch('/api/absopt?w=' + (absM?.deep === undefined ? 'aoSHORT' : 'aoDEEP'));"
    "Object.prototype.toString.call(window.__tfAbsTag); fetch('/api/abstag?w=atRAN');"
    "var trM = 'trNOW'; new Promise(function (r) { r(Math[window.__tfThenSrc]); }).then("
    "function () { fetch('/api/tnrej?w=trFUL'); }, function () { fetch('/api/tnrej?w=' + trM); });"
    "Promise.resolve().then(function () { trM = 'trJOB'; });"
    "</script>"
    "<script>"
    "function absBoom(tag) { throw new Error('absBOOM' + tag); }"
    "if (window.__tfAbsentGate) { fetch('/api/abschosen'); absBoom('ARM'); }"
    " else { fetch('/api/absobserved'); absBoom('OBS'); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
