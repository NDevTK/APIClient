/* THE WEB IDL ARGUMENT COERCION, as one machine every member shares — see idl_args.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#define ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#include "quickjs.h"

/* A member's body, run once its declared arguments are real strings. Same shape as JS_CFUNC_generic_magic, so
   an existing body becomes one by taking a magic it may ignore. */
typedef JSValue (*IdlBody)(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);

/* THE ARGUMENT'S IDL TYPE — what the spec says to convert it to, one entry per position. A bitmask of "which
   ones are strings" was a catch-all: setTimeout's `timeout` is a `long` and its `handler` is a
   `(DOMString or Function)` union, and neither is expressible as "string or not". A member declares the types
   its IDL actually lists. */
typedef enum {
    IDL_ANY = 0,          /* passed through unconverted (`any`, an interface type, a callback) */
    IDL_DOMSTRING,        /* ToString — the page's toString may run */
    /* `DOMString?`. Web IDL converts null AND undefined to the IDL value null before ToString is ever reached,
       so the body receives JS_NULL and never the string "null". textContent is the member that makes this
       load-bearing: `el.textContent = null` is "replace all with null", which removes the children and adds NO
       Text node, and stringifying it wrote the four characters `null` into the page's DOM instead. */
    IDL_DOMSTRING_NULLABLE,
    IDL_LONG,             /* ToNumber, then the integer conversion — the page's valueOf may run */
    /* A `(DOMString or Function)` union, which is TimerHandler and nothing else so far: callable crosses as
       itself, anything else is a DOMString. Named for the rule rather than for the member, because the rule is
       what the IDL states. */
    IDL_STRING_UNLESS_CALLABLE,
    /* A DICTIONARY whose members are all `boolean` defaulting to false — which is every dictionary this engine
       takes: EventInit {bubbles, cancelable, composed}, AddEventListenerOptions {capture, once, passive},
       GetRootNodeOptions {composed}. Web IDL converts one by READING each declared member IN ORDER, and a read
       is one accessor or Proxy trap away from being the page's code, so it is a request exactly like a
       coercion. The body receives a plain engine-built object carrying the converted booleans, which it reads
       with an ordinary property get because nothing of the page's is on it.
       The member NAMES are declared beside the types — see idl_method_id_dict. */
    IDL_DICT_BOOLS,
} IdlArgType;

/* A position the IDL does not list is passed through unconverted, which is what a variadic `any...` tail means
   and what an optional argument beyond the listed ones means. `nargs` is how many the IDL lists. */
#define IDL_MAX_DECLARED 4

/* DECLARE a member: the IDL types of its arguments, and the body to run once they are converted. Returns the
   step id, which the caller CACHES. Registration and installation are separate on purpose: Element's members
   are installed on every wrapper the tree hands out, so registering there would mint a definition per element. */
int  idl_method_id(JSContext *ctx, const IdlArgType *types, int nargs, IdlBody body, int magic);

/* The same declaration for a member that takes an IDL_DICT_BOOLS argument: `dict_members` is a NULL-terminated
   list of the dictionary's member names, in the order the IDL declares them, which is the order Web IDL reads
   them in. A member declares AT MOST ONE dictionary argument — every one in the platform does, and a second
   would need its own cursor rather than sharing this one, which is a DCHECK rather than a silent second read. */
#define IDL_MAX_DICT 6
int  idl_method_id_dict(JSContext *ctx, const IdlArgType *types, int nargs,
                        const char *const *dict_members, IdlBody body, int magic);

/* Release what the pool interned — the dictionary member atoms. */
void idl_args_free(JSContext *ctx);

/* A SETTER's body, run once the assigned value has been converted. A setter is delivered differently from a
   method — one value, no argument vector — so it declares separately rather than being squeezed into the
   method shape. */
typedef JSValue (*IdlSetter)(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic);

/* DECLARE an attribute setter: the IDL type of the value it takes, and the body to run once converted.
   `[LegacyNullToEmptyString]` is the innerHTML/textContent spelling and is part of the TYPE, not the body. */
int  idl_setter_id(JSContext *ctx, IdlArgType type, bool null_to_empty, IdlSetter body, int magic);

/* An attribute GETTER. It takes a magic exactly as a body does, because a reflected attribute is ONE function
   over a table of names and the getters that need no magic simply ignore it. A getter runs none of the page's
   code — it reads the component's own tree — so it is an ordinary C function and not a machine. */
typedef JSValue (*IdlGetter)(JSContext *ctx, JSValueConst this_val, int magic);

/* Install a declared attribute: `getter` may be NULL for a write-only one, `setter_stepid` -1 for read-only. */
void idl_install_accessor(JSContext *ctx, JSValueConst target, const char *name,
                          IdlGetter getter, int getter_magic, int setter_stepid);

/* Install a declared member on `target`. The coercion is a request, so a page's `toString` — loop, await and
   all — suspends and resumes at the exact argument it was on. */
void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid);

#endif
