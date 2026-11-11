/* THE SLOT-OVER-UNKNOWN DOCUMENT: a built-in's receiver check over an unknown carrier, ECMAScript §10.1.15
   RequireInternalSlot ( obj, internalSlot ) and the This*Value operations (§20.4.3.4.1 ThisSymbolValue ( arg ),
   §21.1.3.7.1 ThisNumberValue ( arg )). The two shapes are qs's stringify as a recording snippet bundles it:
   `Symbol.prototype.toString.call(e)` on the arm where `typeof e` is "symbol", and
   `Date.prototype.toISOString.call(e)` on the arm where `e instanceof Date` held over an unknown's prototype
   chain, which may or may not hold [[DateValue]] (`Object.create(Date.prototype)` holds none). The third is
   Number.prototype.valueOf over a carrier `typeof` narrowed to a String, which holds no [[NumberData]].
   Each carrier is an absent global with no example. Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SLOTUNK_H
#define ENGINE_HOST_FIXTURE_SLOTUNK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SLOTUNK[] =
    "<!doctype html><html><body>"
    "<script>"
    "var suS = window.__tfSlotSym, suR = 'suNONE';"
    "if (typeof suS === 'symbol') {"
    "  try { suR = typeof Symbol.prototype.toString.call(suS) === 'string' ? 'suSTRING' : 'suOTHER'; }"
    "  catch (e) { suR = 'suTHREW'; }"
    "}"
    "fetch('/api/su-sym?w=' + suR);"
    "var suD = window.__tfSlotDate, suI = 'suNONE';"
    "if (suD instanceof Date) {"
    "  try { suI = typeof Date.prototype.toISOString.call(suD) === 'string' ? 'suSTRING' : 'suOTHER'; }"
    "  catch (e) { suI = e instanceof TypeError ? 'suTYPE' : e instanceof RangeError ? 'suRANGE' : 'suOTHER'; }"
    "}"
    "fetch('/api/su-date?w=' + suI);"
    "var suN = window.__tfSlotNum, suV = 'suNONE';"
    "if (typeof suN === 'string') {"
    "  try { Number.prototype.valueOf.call(suN); suV = 'suVALUE'; }"
    "  catch (e) { suV = e instanceof TypeError ? 'suTYPE' : 'suOTHER'; }"
    "}"
    "fetch('/api/su-num?w=' + suV);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
