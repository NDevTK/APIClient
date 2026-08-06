/* THE WEB IDL ARGUMENT COERCION, as one machine every member shares — see idl_args.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#define ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"
#include "quickjs-step.h"

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
    /* `ByteString`. ToString and then §3.2.10's RANGE: every code point must be 0x00..0xFF, and one above that
       is a TypeError. That range IS the type — `new Response("", {statusText: "\u0100"})` throws — and it is
       stated here so no body has to remember it. */
    IDL_BYTESTRING,
    /* `USVString`. ToString and then §3.2.11's SCALAR VALUE conversion: every unpaired surrogate becomes
       U+FFFD. That replacement is the whole of what makes the type different from a DOMString, and every
       member of the URL surface takes one. */
    IDL_USVSTRING,
    /* `DOMString?`. Web IDL converts null AND undefined to the IDL value null before ToString is ever reached,
       so the body receives JS_NULL and never the string "null". textContent is the member that makes this
       load-bearing: `el.textContent = null` is "replace all with null", which removes the children and adds NO
       Text node, and stringifying it wrote the four characters `null` into the page's DOM instead. */
    IDL_DOMSTRING_NULLABLE,
    /* THE INTEGER TYPES, each stating its WIDTH and its SIGN — which is all §3.2's conversion needs, and the
       whole of what tells them apart. Every one of them is ToNumber (the page's valueOf may run) and then the
       one arithmetic in idl_args.c: sign(x)·floor(|x|) taken MODULO 2^width, folded into range if the type is
       signed, with a non-finite value becoming 0.
       IT IS A MODULO AND NOT A CLAMP, for all of them. `new Response("", {status: 65736})` is status 200 in
       every browser, and `list.item(2**32)` is item 0 — a saturating conversion answers 65736 and 4294967296,
       and the range check that follows then throws or misses where the spec wraps. */
    IDL_LONG,             /* `long` — 32, signed */
    IDL_UNSIGNED_LONG,    /* `unsigned long` — 32, unsigned */
    IDL_UNSIGNED_SHORT,   /* `unsigned short` — 16, unsigned */
    IDL_LONG_LONG,        /* `long long` — 64, signed */
    /* `[Clamp] long long`. The extended attribute REPLACES the modulo with §3.2.4.2: clamp to the type's range
       and round to the NEAREST integer, choosing the even one at a half. Blob's `slice(start, end)` is the
       member that carries it, and `slice(1.5)` starting at byte 2 rather than byte 1 is the difference. */
    IDL_LONG_LONG_CLAMP,
    /* `unrestricted double` — §3.2.7. ToNumber and nothing else: NaN and the infinities are VALUES of this
       type, which is exactly why QueuingStrategy's `highWaterMark` is declared with it and why the stream's
       own RangeError for a NaN mark has to be the STREAM's check rather than the type's. */
    IDL_UNRESTRICTED_DOUBLE,
    /* A CALLBACK FUNCTION type — §3.2.22. The conversion is a brand check and nothing more: a callable crosses
       as itself and anything else is a TypeError. Declared rather than checked in the body because an optional
       member that is absent must NOT be rejected, and every body that wrote that test by hand is a body that
       can get it wrong once. */
    IDL_CALLBACK,
    /* AN ENUMERATION — §3.2.19. ToString, and then the result must be one of the values the IDL lists or it is
       a TypeError: `new Blob([], {endings: "bogus"})` throws, and an unrecognised value is never silently the
       default. The values are declared beside the member, because they are part of the type. */
    IDL_ENUM,
    /* A `(DOMString or Function)` union, which is TimerHandler and nothing else so far: callable crosses as
       itself, anything else is a DOMString. Named for the rule rather than for the member, because the rule is
       what the IDL states. */
    IDL_STRING_UNLESS_CALLABLE,
    IDL_BOOLEAN,          /* ToBoolean. The conversion runs nothing; the READ that precedes it is the page's */
    /* A DICTIONARY. Web IDL converts one by READING each declared member IN ORDER and converting each by ITS
       OWN type — so a dictionary is that member list plus this very machine, not a second kind of thing. A read
       is one accessor or Proxy trap away from being the page's code, and so is each member's conversion, so
       both are requests exactly like an argument's. The body receives a plain engine-built object carrying the
       converted members, which it reads with an ordinary property get because nothing of the page's is on it.
       The members are declared beside the types — see idl_method_id_dict. */
    IDL_DICT,
    /* `(AddEventListenerOptions or boolean)` — the one union of this shape in the DOM, and its rule is what
       §2.7's "flatten" states: a value that is NOT an object IS the first declared member's boolean, and an
       object is read as an ordinary dictionary. Named for the rule rather than for the member, because the
       rule is what the IDL states — the same reason IDL_STRING_UNLESS_CALLABLE is named that way. */
    IDL_DICT_OR_BOOL_FIRST,
    /* `(T or DOMString)` where T is an INTERFACE type — the union §4.2.4 writes for every member that takes
       "a node or some text", and `el.append('hi')` is the ordinary way to write the second half. Its rule is a
       brand check: an object of the interface's CLASS crosses as itself, anything else is a DOMString. The
       class is declared beside the type, so this file needs to know nothing about what a Node is. */
    IDL_STRING_UNLESS_IFACE,
    /* `BodyInit?` — Fetch's `(ReadableStream or Blob or BufferSource or FormData or URLSearchParams or
       USVString)?`. Its rule is a BRAND check like the two above, but against the BUFFER SOURCE shape rather
       than one class: an ArrayBuffer or any ArrayBufferView crosses as itself, null and undefined are the IDL
       null, and everything else is the union's USVString arm. Blob, FormData and URLSearchParams are brand
       tests beside it now that those interfaces exist; ReadableStream is the one arm still absent, and it
       becomes one more test in this same place. The body learns nothing either way — §5.1's extraction reads
       the arm back off the value. */
    IDL_BODYINIT_NULLABLE,
    /* `sequence<BlobPart>` — §3.2.20's iterator-protocol conversion with `(BufferSource or Blob or USVString)`
       as the element type. Named for the IDL type it IS, the way IDL_BODYINIT_NULLABLE is: the union's brand
       test lives in the one place the union is stated, and the member that takes it learns nothing.
       IT IS A DECLARED TYPE and not something a body walks, because Web IDL converts arguments LEFT TO RIGHT —
       a sequence driven from the body runs after every later argument's conversion, which is observable the
       moment a later argument is a dictionary with a getter on it. */
    IDL_SEQUENCE_BLOBPART,
    /* `BufferSource` — `(ArrayBuffer or ArrayBufferView)`, §3.2.25. An ArrayBuffer, a typed array or a DataView
       crosses as itself and anything else is a TypeError, which is a check the body must not make: written by
       hand it was right twice and wrong the third time, where a plain object reached JS_GetArrayBufferView and
       tripped the engine's own "this is a typed array" assertion. */
    IDL_BUFFERSOURCE,
} IdlArgType;

/* A DICTIONARY MEMBER, as its IDL declares it: the name, the type of its value, and whether the IDL marks it
   `required` (an absent required member is a TypeError, and for a dictionary `undefined` IS absent). A member
   with no `required` written is optional, which is what leaving the field off an initialiser gives. */
/* `values` is the NULL-terminated list an IDL_ENUM member's IDL lists, and is read by nothing else.
   `level` is WHICH DICTIONARY IN THE INHERITANCE CHAIN declares the member — 0 for the most-derived one's
   BASE, counting up to the dictionary itself. §3.2.18 reads the INHERITED members first and each dictionary's
   own members lexicographically among themselves, so `FilePropertyBag : BlobPropertyBag` reads endings, type,
   then lastModified — an order no single sorted list produces, because `lastModified` sorts before `type`.
   Stating the level is what lets the declaration express that AND still be checkable. */
typedef struct {
    const char *name;
    IdlArgType  type;
    bool        required;
    const char *const *values;
    uint8_t     level;
} IdlDictMember;

/* A position the IDL does not list is passed through unconverted, which is what a variadic `any...` tail means
   and what an optional argument beyond the listed ones means. `nargs` is how many the IDL lists. */
#define IDL_MAX_DECLARED 4

/* DECLARE a member: the IDL types of its arguments, and the body to run once they are converted. Returns the
   step id, which the caller CACHES. Registration and installation are separate on purpose: Element's members
   are installed on every wrapper the tree hands out, so registering there would mint a definition per element. */
int  idl_method_id(JSContext *ctx, const IdlArgType *types, int nargs, IdlBody body, int magic);

/* §3.2.10's ByteString RANGE over UTF-8 bytes: true when every code point is 0x00..0xFF. Public because a
   conversion that happens OUTSIDE this machine needs the same answer — Headers' fill converts a record's keys
   itself, one [[Get]] at a time, and the range is the type's rule rather than that component's. */
bool idl_is_bytestring(const char *utf8, size_t len);

/* The same declaration for a member whose IDL tail is VARIADIC, and/or that takes an interface-or-string
   union. `variadic` makes the LAST declared type apply to every argument from there on, which is what a `T...`
   tail means — stated by the member rather than assumed for all of them, because assuming it once converted
   addEventListener's CALLBACK to a string. `iface` is the class an object must be to cross an
   IDL_STRING_UNLESS_IFACE position as itself. */
int  idl_method_id_ext(JSContext *ctx, const IdlArgType *types, int nargs, bool variadic, JSClassID iface,
                       IdlBody body, int magic);

/* The same declaration for a member that takes an IDL_DICT argument: `members` lists the dictionary's members
   in the order the IDL declares them, which is the order Web IDL reads them in. A member declares AT MOST ONE
   dictionary argument — every one in the platform does, and a second would need its own cursor rather than
   sharing this one, which is a DCHECK rather than a silent second read.
   THERE IS NO CEILING ON `nmembers`. There was one, at six, and RequestInit's eleven walked past it — the same
   ceiling-as-detector this pool already replaced once. `members` must outlive the declaration; every caller
   passes a static, which is what lets the pool keep the pointer rather than a copy. */
int  idl_method_id_dict(JSContext *ctx, const IdlArgType *types, int nargs,
                        const IdlDictMember *members, int nmembers, IdlBody body, int magic);

/* A MEMBER WHOSE ALGORITHM RUNS THE PAGE'S CODE AFTER ITS ARGUMENTS ARE CONVERTED. customElements.define is
   the first: §4.13.4 reads `constructor.observedAttributes`, which is a static GETTER, and then converts what
   it got — the page's code, twice, after every declared argument is already a real value. A plain body cannot
   do that: reaching for JS_GetProperty there is a C activation hosting the page's loops, which is the
   drive-to-completion this engine aborts on. Declaring the conversions and hand-rolling them so the member can
   be a machine is the other wrong answer — that is the duplication one machine exists to prevent.
   So the body is itself a STEP, with the converted arguments in place: same return contract as a
   JSTrampStepDef's step, and its own state, whose SIZE the member declares and whose owned values its visit
   names. The state is zeroed before the first entry, so a stage byte of 0 means "not started".
   `presult` is where it leaves the member's answer. */
typedef int (*IdlStepBody)(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc);
/* The state's OWNERSHIP contract, and the two things every step state must state: what it holds (traced for the
   GC and cloned at a deep fork) and how to release it (at teardown, including the throw path). */
typedef struct {
    IdlStepBody body;
    size_t      state_size;
    void      (*visit)(JSContext *ctx, void *state, JSStepVisit *v);
    void      (*release)(JSContext *ctx, void *state);
} IdlStepDecl;
/* DECLARE WHERE THE OPTIONAL ARGUMENTS START. §3.6.2 makes an `undefined` passed for an optional argument with
   no default mean the argument is ABSENT — `new URL("aaa:b", undefined)` is a one-argument call, and
   converting that undefined would give the base URL the string "undefined" and throw. Set after the
   declaration — it names the member the LAST one made, the way idl_method_id_ext sets `variadic`, because the
   id a declaration returns is the RUNTIME's step id and not this pool's index. A member that never calls this
   converts every declared position, which is right for a member whose arguments are all required. */
void idl_optional_from(int first_optional);

int idl_method_id_step(JSContext *ctx, const IdlArgType *types, int nargs,
                       const IdlDictMember *members, int nmembers,
                       const IdlStepDecl *decl, int magic);

/* THE MAGIC THIS INVOCATION WAS DECLARED WITH. A plain body takes it as an argument; a step body cannot, because
   its signature is the step contract and that is shared with every machine in the engine. It is read off the
   header instead, which is the same place the receiver and the arguments come from — one declaration serving two
   members (innerHTML and outerHTML are one walk with two starting points) is exactly what a magic is for. */
int idl_step_magic(const JSStepHdr *hdr);

/* READ a member of the dictionary the declaration built. An `optional D options = {}` argument that the page
   did not pass is not there at all, so a body that reads it with JS_GetPropertyStr calls a property get on
   `undefined` — a pending TypeError, and a truthy JS_EXCEPTION where a `false` belonged. That is a mistake per
   BODY, which is the thing this machine exists to have only one of, so reading a dictionary is part of the
   declaration's contract: an absent dictionary has every member absent, and that is all it means.
   Nothing of the page's is on the object these read, so neither runs any of its code. */
JSValue idl_dict_get(JSContext *ctx, JSValueConst dict, const char *name);
bool    idl_dict_bool(JSContext *ctx, JSValueConst dict, const char *name);

/* §4.2.3'S TREE STEPS, DRAINED WHERE THEY CAN YIELD.
   A DOM mutation's insertion/removing steps are a walk of the whole changed subtree, and they used to run inside
   the mutation chokepoint — inside a C member body, the deepest place in this engine with no way to suspend. The
   spec runs them SYNCHRONOUSLY as part of the insertion, so they cannot become a deferred job: a page that
   appends an element and then calls a method its upgrade installed depends on the ordering. So the DOM layer
   records what changed and this machine drains the record before the member returns — no page code in between,
   the spec's ordering intact, and a walk that yields per node.
   IT IS DRAINED HERE AND NOWHERE ELSE, because this is the one point every declared member converges on. A
   per-member drain would be per-member plumbing, and every member added afterwards would be a silent gap — the
   same shape as a dispatch predicate at each call site. A member that mutates the tree WITHOUT being declared
   leaves a record nobody consumes, which the machine asserts on rather than letting it rot.
   The DOM layer registers these because this file must not know what a Node is; it knows only that something
   was recorded and that it takes N steps to consume. */
typedef struct {
    void *(*take)(JSContext *ctx);                 /* everything recorded so far, or NULL; leaves none behind */
    bool  (*step)(JSContext *ctx, void *buf);      /* ONE node; true while more remains */
    void  (*release)(JSContext *ctx, void *buf);
    bool  (*recorded)(void);                       /* is anything waiting to be taken */
} IdlTreeSteps;
void idl_set_tree_steps(const IdlTreeSteps *ops);

/* THE DOCUMENT'S INSTALL IS DONE — no further member declaration can be correct. A component declares in its
   init and installs from the cached id, so a declaration reached from a wrapper or from a running flow is the
   per-object mint this asserts against. Called once by the entry, after the components are installed. */
void idl_args_seal(void);

/* Release what the pool interned — the dictionary member atoms — and the pool's blocks. */
void idl_args_free(JSContext *ctx);

/* A SETTER's body, run once the assigned value has been converted. A setter is delivered differently from a
   method — one value, no argument vector — so it declares separately rather than being squeezed into the
   method shape. */
typedef JSValue (*IdlSetter)(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic);

/* DECLARE an attribute setter: the IDL type of the value it takes, and the body to run once converted.
   `[LegacyNullToEmptyString]` is the innerHTML/textContent spelling and is part of the TYPE, not the body. */
int  idl_setter_id(JSContext *ctx, IdlArgType type, bool null_to_empty, IdlSetter body, int magic);

/* AN ATTRIBUTE SETTER WHOSE ALGORITHM IS A MACHINE — the setter shape of idl_method_id_step. `innerHTML =` is
   the member that needs it: assigning markup PARSES it, and a parse is work of the page's size, so the body has
   to be able to yield. It carries `null_to_empty` for the same reason the plain setter does — the
   [LegacyNullToEmptyString] is part of the TYPE, so no body has to remember it. */
int  idl_setter_id_step(JSContext *ctx, IdlArgType type, bool null_to_empty, const IdlStepDecl *decl, int magic);

/* An attribute GETTER. It takes a magic exactly as a body does, because a reflected attribute is ONE function
   over a table of names and the getters that need no magic simply ignore it. A getter runs none of the page's
   code — it reads the component's own tree — so it is an ordinary C function and not a machine. */
typedef JSValue (*IdlGetter)(JSContext *ctx, JSValueConst this_val, int magic);

/* Install a declared attribute: `getter` may be NULL for a write-only one, `setter_stepid` -1 for read-only. */
/* THE SLOWEST SINGLE IDL-MEMBER STEP since the last reset, and which member it was. A step machine's contract
   is that one step is short, so this is how a scheduler assertion that can only say "this flow went N ms
   without offering a suspend point" finds out what the flow was inside. Every declared member passes through
   the one args machine, so a native call that never returned names itself here — and a small answer says the
   culprit is not an IDL member, which is equally an answer. Dev-only; a release build reports 0. */
/* THE ONE MINT for a step member's function value, and the only thing that can name its pool entry. Use it
   instead of JS_NewCFunction2(..., JS_CFUNC_step, stepid) — a hand-written copy leaves the member anonymous in
   every diagnostic, and there is nothing to notice that until one of them is the thing you are looking for. */
JSValue idl_step_function(JSContext *ctx, const char *name, int length, int stepid);
JSValue idl_step_constructor(JSContext *ctx, const char *name, int length, int stepid);

void idl_slowest_reset(void);
int64_t idl_slowest_step(const char **name);
/* The same window's TOTAL across every member step, and how many there were. The max alone cannot separate one
   very slow call from very many short ones. */
int64_t idl_step_total(long *count);

/* §3.7.3's @@toStringTag on an interface PROTOTYPE object: the interface's identifier, non-writable,
   non-enumerable, configurable. Every interface prototype has one, so every interface calls this. */
void idl_interface_tag(JSContext *ctx, JSValueConst proto, const char *iface);

void idl_install_accessor(JSContext *ctx, JSValueConst target, const char *name,
                          IdlGetter getter, int getter_magic, int setter_stepid);

/* AN ACCESSOR WHOSE GETTER IS A MACHINE. A getter takes no arguments, so it has nothing to CONVERT — and that
   is why it was a plain C function, which was the wrong conclusion: it may still have work of the PAGE'S size
   to do. `innerHTML` serialises the whole document, `outerHTML` the same, `childNodes.length` counts every
   child; none of it can reach the page's code and all of it held the scheduler for as long as the tree was big.
   A getter declared this way is a function object of exactly the kind the SETTER already is, called the same
   way by the property machinery, so it can yield at every step of its walk.
   The plain-C form above is what remains to be converted, not a second way of doing this. */
int  idl_getter_id_step(JSContext *ctx, const IdlStepDecl *decl, int magic);
void idl_install_accessor_step(JSContext *ctx, JSValueConst target, const char *name,
                               int getter_stepid, int setter_stepid);

/* Install a declared member on `target`. The coercion is a request, so a page's `toString` — loop, await and
   all — suspends and resumes at the exact argument it was on. */
void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid);
/* The installer for a method whose algorithm is a step machine of its OWN (its own JSTrampStepDef) rather than
   a member of the args machine — `click` and `dispatchEvent` today. Separate from idl_install_method because
   they are separate things, and each asserts it was handed its own kind. */
void idl_install_step_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid);

#endif
