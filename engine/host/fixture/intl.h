/* THE INTL DOCUMENT: the environment ECMAScript and ECMA-402 leave to the implementation, answered by the Intl
   host. Date's local time is the pinned system time zone (core/intl/intl_environment.h's UTC), whatever zone the
   operating system the engine runs on is set to; the three services and the namespace's functions answer
   requests whose answers are stable across ICU and CLDR versions, so the native host and the WASM host's V8 agree
   on them; the last is formatted under navigator.language, an environment value whose example is the default
   locale, so its record is derived. One of test_forced.c's exploring documents (TF_DOCS); it ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_INTL_H
#define ENGINE_HOST_FIXTURE_INTL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_INTL[] =
    "<!doctype html><html><body>"
    "<script>"
    "var lt = new Date(Date.UTC(2024, 6, 2, 3, 4));"
    "fetch('/api/intltz?off=' + lt.getTimezoneOffset() + '&h=' + lt.getHours() + '&m=' + lt.getMinutes() +"
    " '&set=' + new Date(2024, 0, 2, 3).getTime());"
    "var ic = new Intl.Collator();"
    "fetch('/api/intlcol?s=' + ['b', 'a', 'C'].sort(ic.compare).join('') +"
    " '&base=' + new Intl.Collator('en', { sensitivity: 'base' }).compare('a', 'A') +"
    " '&num=' + ['10', '9'].sort(new Intl.Collator(void 0, { numeric: true }).compare).join('-') +"
    " '&loc=' + ic.resolvedOptions().locale);"
    "var inf = new Intl.NumberFormat(void 0, { maximumSignificantDigits: 5 });"
    "fetch('/api/intlnf?sig=' + inf.format(1234.56789) +"
    " '&cur=' + new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' }).format(12.5) +"
    " '&big=' + new Intl.NumberFormat('en-US', { useGrouping: false }).format(12345678901234567890n) +"
    " '&parts=' + inf.formatToParts(-1234.5).map(function (p) { return p.type; }).join('.'));"
    "var idt = Intl.DateTimeFormat();"
    "fetch('/api/intldtf?tz=' + idt.resolvedOptions().timeZone +"
    " '&d=' + new Intl.DateTimeFormat('en-US', { timeZone: 'UTC' }).format(Date.UTC(2024, 0, 2)) +"
    " '&parts=' + new Intl.DateTimeFormat('en-US', { timeZone: 'UTC', year: 'numeric', month: '2-digit' })"
    ".formatToParts(Date.UTC(2024, 0, 2)).map(function (p) { return p.type; }).join('.') +"
    " '&hc=' + new Intl.DateTimeFormat('en-US', { hour: 'numeric', hour12: false }).resolvedOptions().hourCycle);"
    "fetch('/api/intlsvo?tz=' + Intl.supportedValuesOf('timeZone').includes('America/New_York') +"
    " '&units=' + Intl.supportedValuesOf('unit').length + '&rtf=' + typeof Intl.RelativeTimeFormat +"
    " '&tag=' + Object.prototype.toString.call(Intl).slice(8, -1) +"
    " '&canon=' + Intl.getCanonicalLocales(['EN-us', 'en-US']).join('_') +"
    " '&slo=' + Intl.NumberFormat.supportedLocalesOf(['zz', 'de']).join('_'));"
    "fetch('/api/intlder?v=' + new Intl.NumberFormat(navigator.language).format(1234.5));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
