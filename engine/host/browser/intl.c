/* Intl formatters — see intl.h. Built from the IDL driver (idl.h). Every operation returns locale-formatted
 * output that is genuinely unknown headless (no ICU/locale) -> the opaque concolic value, so the interface
 * shape is declared while the values are the honest unknown. One IDL covers the Intl formatter surface
 * (NumberFormat/DateTimeFormat/RelativeTimeFormat/Collator/ListFormat/DisplayNames/PluralRules/Segmenter). */
#include "intl.h"
#include "idl.h"
#include "opaque.h"   /* js_opaque_stub — an operation whose result is unknown headless */

static const IDLMember INTL_IDL[] = {
    { "format",              IDL_METHOD, js_opaque_stub, 1 },
    { "formatToParts",       IDL_METHOD, js_opaque_stub, 1 },
    { "formatRange",         IDL_METHOD, js_opaque_stub, 2 },
    { "formatRangeToParts",  IDL_METHOD, js_opaque_stub, 2 },
    { "resolvedOptions",     IDL_METHOD, js_opaque_stub, 0 },
    { "select",              IDL_METHOD, js_opaque_stub, 1 },   /* PluralRules */
    { "compare",             IDL_METHOD, js_opaque_stub, 2 },   /* Collator */
    { "of",                  IDL_METHOD, js_opaque_stub, 1 },   /* DisplayNames */
};

JSValue js_intl_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    return idl_instance(ctx, INTL_IDL, sizeof INTL_IDL / sizeof INTL_IDL[0]);
}
