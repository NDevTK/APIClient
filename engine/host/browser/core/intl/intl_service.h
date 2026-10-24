/* The ECMA-402 abstract operations every Intl service shares, as sub-sequences of a step machine: §9.2.1
 * CanonicalizeLocaleList, §9.2.10 CoerceOptionsToObject, §9.2.11 GetOption and the number options, the host
 * question each service asks with what it read, and the slot record an instance's resolved state lives in.
 *
 * A value the page supplied may be unknown external input. A read whose answer is decided by a list of values
 * forks over that list (the spec's own partition, RangeError included); a value with no list is computed from
 * the unknown's example and the unknown is kept as an operand, so every answer the instance later gives is
 * derived from it. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_INTL_SERVICE_H
#define ENGINE_HOST_BROWSER_CORE_INTL_INTL_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs-step.h"
#include "core/intl/intl_host.h"

/* The services this engine builds, in the order the hosts' "service" field names them. */
typedef enum { INTL_SVC_COLLATOR, INTL_SVC_NUMBER_FORMAT, INTL_SVC_DATE_TIME_FORMAT, INTL_SVC_N } IntlService;

/* Once per agent, from intl.c: the slot record key and the names the operations intern. */
void intl_service_init(JSContext *ctx);
void intl_service_free(JSRuntime *rt);
/* `svc`'s supportedLocalesOf (§10.2.2, §11.2.2, §16.2.2) on its constructor. Defined in intl.c. */
void intl_install_supported_locales_of(JSContext *ctx, JSValueConst ctor, IntlService svc);

/* What every Intl machine holds beside its header. Every member is a JS value, owned, so intl_state_visit names
   all of them and a fork copies the whole state. */
typedef struct {
    JSStepHdr hdr;        /* first: the driver writes through it */
    JSValue requested;    /* §9.2.1's List, as an Array of canonicalized tags */
    JSValue options;      /* the options object after its coercion */
    JSValue read;         /* the option values read so far, by property name: what the host is asked with */
    JSValue deps;         /* the unknown operands whose examples were computed with, as an Array */
    JSValue obj;          /* the object the machine is building or answering */
    JSValue slots;        /* the instance's resolved record */
    JSValue result;       /* the completion, until fini takes it */
    JSValue held;         /* the value a sub-sequence read and has not yet converted */
    JSValue list;         /* §9.2.1's localesObj */
    JSValue opt;          /* §9.2.8's resolution options record, by extension key */
    JSValue raw;          /* option values read with a bare Get, converted later (§16.1.2) */
    int64_t k, len;       /* §9.2.1's index and length */
    uint8_t phase;        /* which half of the sub-sequence in flight (read, then convert) */
    uint8_t started;
} IntlState;

/* The cursors a stage holding an Intl sub-sequence hands STEP_GOTO. */
#define INTL_CURSORS(s) &(s)->phase, &(s)->hdr.get_phase, &(s)->hdr.str_phase, &(s)->hdr.num_phase, NULL

void intl_state_start(IntlState *s);
void intl_state_visit(JSContext *ctx, IntlState *s, JSStepVisit *v);
JSValue intl_state_fini(IntlState *s, bool take_result);

/* §9.2.1 CanonicalizeLocaleList ( locales ) into s->requested. 0 done, -1 threw, else a step code to return. */
int intl_canonicalize_locale_list(JSContext *ctx, IntlState *s, JSValueConst locales, JSValue in,
                                  JSValue **out_cb, int *out_argc);
/* §9.2.10 CoerceOptionsToObject ( options ) into s->options; -1 threw. */
int intl_coerce_options(JSContext *ctx, IntlState *s, JSValueConst options);

typedef enum { INTL_OPT_STRING, INTL_OPT_BOOLEAN } IntlOptType;
/* One GetOption: the property, its type, its values (NULL-terminated, or NULL for none) and its default (NULL
   for undefined; a boolean option's default is "true" or "false"). */
typedef struct {
    const char *prop;
    IntlOptType type;
    const char *const *values;
    const char *dflt;
    const char *fork_op;  /* the fork's name, for a read over unknown input: the spec step that reads it */
} IntlOption;

/* §9.2.11 GetOption ( options, propertyKey, type, values, default ) over s->options, recording the answer in
   s->read under its property. *pout is the answer, owned. 0 done, -1 threw, else a step code. */
int intl_get_option(JSContext *ctx, IntlState *s, const IntlOption *o, JSValue in, JSValue *pout,
                    JSValue **out_cb, int *out_argc);
/* §9.2.14 GetNumberOption ( options, property, minimum, maximum, fallback ), recorded the same way; a fallback
   of NAN is undefined, and *pout is then NAN for an absent option. */
int intl_get_number_option(JSContext *ctx, IntlState *s, const char *prop, double min, double max,
                           double fallback, JSValue in, double *pout, JSValue **out_cb, int *out_argc);
/* ECMAScript §10.1.13 OrdinaryCreateFromConstructor ( ctor, intrinsicDefaultProto [ , internalSlotsList ] ) into
   s->obj, an object of `cls` whose prototype is §10.1.14 GetPrototypeFromConstructor's: newTarget's "prototype"
   when that is an Object (a Get, so a request), the class's own in this realm otherwise. 0 done, -1 threw. */
int intl_create_from_constructor(JSContext *ctx, IntlState *s, JSValueConst new_target, JSClassID cls, JSValue in,
                                 JSValue **out_cb, int *out_argc);
/* One of §9.2.8 ResolveOptions' resolution option descriptors: GetOption, then step 6.d's ToString and `type`
   nonterminal check, into s->opt under `key`. */
int intl_resolution_option(JSContext *ctx, IntlState *s, const IntlOption *o, const char *key, JSValue in,
                           JSValue **out_cb, int *out_argc);
/* ? Get ( options, prop ) into s->raw under `prop`, for a conversion a later step makes. */
int intl_get_raw(JSContext *ctx, IntlState *s, const char *prop, JSValue in, JSValue **out_cb, int *out_argc);
/* §9.2.13 DefaultNumberOption ( value, minimum, maximum, fallback ) over s->raw's `prop`, recorded in s->read. */
int intl_default_number_option(JSContext *ctx, IntlState *s, const char *prop, double min, double max,
                               double fallback, JSValue in, double *pout, JSValue **out_cb, int *out_argc);
/* §9.2.12 GetBooleanOrStringNumberFormatOption over s->options; *pout is a boolean or one of `values`. */
int intl_get_bool_or_string_option(JSContext *ctx, IntlState *s, const char *prop, const char *const *values,
                                   const char *fork_op, JSValue fallback, JSValue in, JSValue *pout,
                                   JSValue **out_cb, int *out_argc);

/* The host's answer to `op`, asked with the tags of `locales` (an Array, or undefined) followed by
   `one_locale` and the default locale where given, the members of `record` named by `keys` (NULL-terminated;
   an undefined member is not sent), and `args`. The answer's `ok` value, or JS_EXCEPTION with the host's
   refusal thrown as the RangeError or TypeError it names. Charged as host work by the bytes it asks with, never
   the answer's, so the charge is the same on both hosts. */
JSValue intl_ask(JSContext *ctx, IntlHostOp op, JSValueConst locales, const char *one_locale, bool with_default,
                 JSValueConst record, const char *const *keys, const IntlHostValue *args, int nargs);

/* The resolved record an instance of `cls` carries, owned, or JS_UNDEFINED when `obj` is not one. */
JSValue intl_slots_of(JSContext *ctx, JSValueConst obj, JSClassID cls);
/* Attach `slots` (consumed) to `obj` as its resolved record. */
void intl_slots_attach(JSContext *ctx, JSValueConst obj, JSValue slots);
/* A member of a slot record, owned. */
JSValue intl_slot(JSContext *ctx, JSValueConst slots, const char *name);
/* `real` (consumed), the host's answer over the instance's examples, as the result of `op`: derived from the
   instance's unknown operands and `inputs` where any is unknown, else `real` itself. */
JSValue intl_derive(JSContext *ctx, const char *op, JSValueConst slots, const JSValueConst *inputs, int ninputs,
                    JSValue real);
/* The instance's unknown operands, from s->deps, into the slot record it is about to carry. */
void intl_slots_take_deps(JSContext *ctx, IntlState *s, JSValueConst slots);

/* A member of s->read, owned. */
JSValue intl_read(JSContext *ctx, IntlState *s, const char *prop);
/* `v` (consumed) as s->read's `prop`, for a value a step computes rather than reads. */
void intl_set_read(JSContext *ctx, IntlState *s, const char *prop, JSValue v);
/* `v` (consumed) as s->opt's `key`: a resolution option a step states rather than reads. */
void intl_set_opt(JSContext *ctx, IntlState *s, const char *key, JSValue v);

/* ECMA-402 §6.2.1 IsWellFormedLanguageTag ( locale ). */
bool intl_is_well_formed_language_tag(const char *tag, size_t len);
/* ECMA-402 §6.3.1 IsWellFormedCurrencyCode ( currency ). */
bool intl_is_well_formed_currency_code(const char *c, size_t len);
/* ECMA-402 §6.6.1 IsWellFormedUnitIdentifier ( unitIdentifier ). */
bool intl_is_well_formed_unit_identifier(const char *u, size_t len);
/* ECMA-402 §6.6.3 AvailableCanonicalUnits ( ), as an Array. */
JSValue intl_available_canonical_units(JSContext *ctx);

/* A host value for a JS string, number or boolean `v`; a string's bytes are borrowed from *owned, which the
   caller frees with JS_FreeCString. False for any other value. */
bool intl_host_value_of(JSContext *ctx, JSValueConst v, IntlHostValue *out, const char **owned);

#endif
