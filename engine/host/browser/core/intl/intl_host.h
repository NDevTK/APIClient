/* The locale and time zone data the engine asks its host for, one question per op. A build links exactly one
 * host: intl_host_icu.c (native, ICU4C) or intl_host_v8.c (WASM, the V8 of the engine's own frame). The engine
 * owns every abstract operation; a host answers only what its data decides.
 *
 * An answer is a malloc'd JSON document the caller frees: `{"ok":<value>}`, or `{"throw":"RangeError",
 * "message":"..."}` where the host's data refuses an input. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_INTL_HOST_H
#define ENGINE_HOST_BROWSER_CORE_INTL_INTL_HOST_H

#include <stdbool.h>
#include <stddef.h>

/* Each op, with the name both hosts and the relay's refusal list spell it by.
 *   TIME_ZONE_OFFSET  args: zone (string), t (number, ms from the epoch). ok: local minus UTC, in ms.
 *   DATA_VERSION      no args. ok: {host, icu, cldr, tz}, for the artifact's stamp. */
#define INTL_HOST_OPS(X)                                \
    X(INTL_OP_TIME_ZONE_OFFSET, "TimeZone.offset")      \
    X(INTL_OP_DATA_VERSION, "Host.dataVersion")

#define INTL_HOST_OP_ENUM(id, name) id,
typedef enum { INTL_HOST_OPS(INTL_HOST_OP_ENUM) INTL_OP_N } IntlHostOp;
#undef INTL_HOST_OP_ENUM

typedef enum { INTL_VALUE_STRING, INTL_VALUE_NUMBER } IntlHostValueKind;

/* One argument. A string is UTF-8 of `len` bytes, borrowed for the call. */
typedef struct {
    IntlHostValueKind kind;
    const char *str;
    size_t len;
    double num;
} IntlHostValue;

typedef struct {
    IntlHostOp op;
    IntlHostValue args[2];
    int nargs;
} IntlHostRequest;

/* The host's answer to `req`, as described above. Owned by the caller. */
char *intl_host_call(const IntlHostRequest *req);
const char *intl_host_op_name(IntlHostOp op);
/* The number in an `{"ok":<number>}` answer to `op`. A refusal or any other shape is a host defect for an op whose
   inputs the engine computed, and is fatal. */
double intl_host_answer_number(const char *answer, IntlHostOp op);

#endif
