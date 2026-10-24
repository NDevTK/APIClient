/* The locale and time zone data the engine asks its host for, one question per op. A build links exactly one
 * host: intl_host_icu.c (native, ICU4C) or intl_host_v8.c (WASM, the V8 of the engine's own frame). The engine
 * owns every abstract operation that reads the page's values; a host answers only what its data decides.
 *
 * An answer is a malloc'd JSON document the caller frees: `{"ok":<value>}`, or `{"throw":"RangeError",
 * "message":"..."}` where the host's data refuses an input. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_INTL_HOST_H
#define ENGINE_HOST_BROWSER_CORE_INTL_INTL_HOST_H

#include <stdbool.h>
#include <stddef.h>

/* Each op, with the name both hosts and the relay spell it by. `locales` is the request's tags in priority order
   (a resolve's end with the default locale); a format op's one locale and fields are the instance's slots.
     LOCALE_CANONICALIZE  args: a well-formed tag. ok: §6.2.2 CanonicalizeUnicodeLocaleId of it.
     LOCALE_SUPPORTED     fields: service, localeMatcher. ok: §9.2.9 FilterLocales over `locales`.
     COLLATOR_RESOLVE     fields: usage, localeMatcher, co, kn, kf. ok: {locale, collation, numeric, caseFirst,
                          sensitivity, ignorePunctuation}, the last two the locale data's defaults.
     NUMBER_RESOLVE       fields: localeMatcher, nu. ok: {locale, numberingSystem}.
     DATE_RESOLVE         fields: localeMatcher, ca, nu, hc (null after hour12). ok: {locale, calendar,
                          numberingSystem, hc, and the locale's hourCycle, hourCycle12, hourCycle24}.
     DATE_PATTERN         fields: the best-format inputs. ok: {hasHour, and the best format's components}. */
/* The questions over values:
     COLLATOR_COMPARE     args: x, y. ok: §10.3.3.1 CompareStrings's -1, 0 or 1.
     CURRENCY_DIGITS      args: currency. ok: §16.5.1 CurrencyDigits.
     NUMBER_FORMAT, NUMBER_TO_PARTS, DATE_FORMAT, DATE_TO_PARTS  args: x. ok: the string, or [{type, value}].
     TIME_ZONE_IDENTIFIER args: zone. ok: §6.5.2's [[PrimaryIdentifier]], or a RangeError.
     TIME_ZONE_OFFSET     args: zone, t (ms). ok: local minus UTC, in ms.
     SUPPORTED_VALUES     args: key. ok: §8.3.2's list.
     DATA_VERSION         no args. ok: {host, icu, cldr, tz}, for the artifact's stamp. */
#define INTL_HOST_OPS(X)                                                \
    X(INTL_OP_LOCALE_CANONICALIZE, "Locale.canonicalize")               \
    X(INTL_OP_LOCALE_SUPPORTED, "Locale.supported")                     \
    X(INTL_OP_COLLATOR_RESOLVE, "Collator.resolve")                     \
    X(INTL_OP_COLLATOR_COMPARE, "Collator.compare")                     \
    X(INTL_OP_NUMBER_RESOLVE, "NumberFormat.resolve")                   \
    X(INTL_OP_CURRENCY_DIGITS, "NumberFormat.currencyDigits")           \
    X(INTL_OP_NUMBER_FORMAT, "NumberFormat.format")                     \
    X(INTL_OP_NUMBER_TO_PARTS, "NumberFormat.formatToParts")            \
    X(INTL_OP_TIME_ZONE_IDENTIFIER, "TimeZone.primaryIdentifier")       \
    X(INTL_OP_TIME_ZONE_OFFSET, "TimeZone.offset")                      \
    X(INTL_OP_DATE_RESOLVE, "DateTimeFormat.resolve")                   \
    X(INTL_OP_DATE_PATTERN, "DateTimeFormat.pattern")                   \
    X(INTL_OP_DATE_FORMAT, "DateTimeFormat.format")                     \
    X(INTL_OP_DATE_TO_PARTS, "DateTimeFormat.formatToParts")            \
    X(INTL_OP_SUPPORTED_VALUES, "Intl.supportedValuesOf")               \
    X(INTL_OP_DATA_VERSION, "Host.dataVersion")

#define INTL_HOST_OP_ENUM(id, name) id,
typedef enum { INTL_HOST_OPS(INTL_HOST_OP_ENUM) INTL_OP_N } IntlHostOp;
#undef INTL_HOST_OP_ENUM

typedef enum { INTL_VALUE_STRING, INTL_VALUE_NUMBER, INTL_VALUE_BOOLEAN, INTL_VALUE_NULL } IntlHostValueKind;

/* One argument or field value. A string is UTF-8 of `len` bytes, borrowed for the call. */
typedef struct {
    IntlHostValueKind kind;
    const char *str;
    size_t len;
    double num;
    bool boolean;
} IntlHostValue;

typedef struct {
    const char *key;
    IntlHostValue value;
} IntlHostField;

typedef struct {
    IntlHostOp op;
    const char *const *locales;
    int nlocales;
    const IntlHostField *fields;
    int nfields;
    IntlHostValue args[2];
    int nargs;
} IntlHostRequest;

/* The host's answer to `req`, as described above. Owned by the caller. */
char *intl_host_call(const IntlHostRequest *req);
const char *intl_host_op_name(IntlHostOp op);
/* The number in an `{"ok":<number>}` answer to `op`. A refusal or any other shape is a host defect for an op whose
   inputs the engine computed, and is fatal. */
double intl_host_answer_number(const char *answer, IntlHostOp op);
/* The field of `req` named `key`, or NULL. */
const IntlHostValue *intl_host_field(const IntlHostRequest *req, const char *key);

#endif
