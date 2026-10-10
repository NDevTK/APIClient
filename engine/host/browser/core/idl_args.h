/* Web IDL argument conversion: one step machine every declared member shares (see idl_args.c). */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#define ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_iter.h"
#include "solver/concolic.h"   /* IDL_DCHECK_MEMBER asks whether a member crossed as unknown input */

/* A member's body, run once its declared arguments are converted. Same shape as JS_CFUNC_generic_magic, so an
   existing body becomes one by taking a magic it may ignore. */
typedef JSValue (*IdlBody)(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);

/* The IDL type a position or dictionary member converts to, one entry per position, stated as the IDL writes
   it. Each row is one conversion; a row exists wherever two IDL types convert differently in a way a page can
   observe, so a row is never replaced by a test in a member's body.

   Each row's rule over unknown external input (cross, fork or ask nothing) is idl_concolic_rule's answer, and a
   row that needs a class, value list, typed-array kind or dictionary names it beside the position or member
   (idl_iface_brand, idl_arg_iface, idl_arg_enum, idl_typed_array, IdlDictMember::dict). */
typedef enum {
    IDL_ANY = 0,          /* `any`: passed through unconverted */
    IDL_DOMSTRING,        /* ToString — the page's toString may run */
    /* `ByteString` — Web IDL §3.2.11 ByteString: ToString, then a code point above 0xFF is a TypeError. */
    IDL_BYTESTRING,
    /* `USVString` — Web IDL §3.2.12 USVString: ToString, then every unpaired surrogate becomes U+FFFD. */
    IDL_USVSTRING,
    /* `DOMString?` — Web IDL §3.2.20 Nullable types: null and undefined become the IDL null and the body
       receives JS_NULL, never the string "null" (`el.textContent = null` removes the children and adds no
       Text node). */
    IDL_DOMSTRING_NULLABLE,
    /* `USVString?` — §3.2.20's null rule followed by §3.2.12's scalar value conversion. XMLHttpRequest's
       `optional USVString? username = null` needs it: `open(m, u, false, null)` leaves the URL's username alone. */
    IDL_USVSTRING_NULLABLE,
    /* The integer types, each stating its width and sign. Each is ToNumber (the page's valueOf may run), then
       Web IDL §3.2.4.9 Abstract operations' ConvertToInt: sign(x)·floor(|x|) modulo 2^width, folded into range
       if signed, a non-finite value becoming 0. It is a modulo, not a clamp: `new Response("", {status: 65736})`
       is status 200 and `list.item(2**32)` is item 0. */
    IDL_LONG,             /* `long` — 32, signed */
    IDL_UNSIGNED_LONG,    /* `unsigned long` — 32, unsigned */
    /* `[EnforceRange] unsigned long` — Web IDL §3.3.6 [EnforceRange] says which types carry it and §3.2.4.9
       Abstract operations says what it does: a non-finite value, or an integer part outside the type's range,
       is a TypeError instead of a wrap. A separate row because the extended attribute is the conversion
       (IndexedDB's `cursor.advance(-1)` is a TypeError, not a 4294967295-record walk). */
    IDL_UNSIGNED_LONG_ENFORCE,
    /* `[EnforceRange] long` — §3.3.6 over Web IDL §3.2.4.5 long. HTML's getImageData/putImageData declare it for
       every coordinate: `ctx.getImageData(0, 0, Infinity, 1)` is a TypeError at the conversion rather than the
       body's "IndexSizeError". */
    IDL_LONG_ENFORCE,
    IDL_UNSIGNED_SHORT,   /* `unsigned short` — 16, unsigned */
    IDL_LONG_LONG,        /* `long long` — 64, signed */
    /* `unsigned long long` — 64, unsigned; a separate row because the sign is observable. File System's
       `truncate(-1)` is 2**64-1 bytes (a QuotaExceededError), never a negative size. */
    IDL_UNSIGNED_LONG_LONG,
    /* `[EnforceRange] unsigned long long` — §3.3.6 over Web IDL §3.2.4.8 unsigned long long. Its upper bound is
       2**53-1, not 2**64-1: ConvertToInt's first step narrows a 64-bit type with [EnforceRange] or [Clamp] to
       what a Number represents exactly. The integer part is taken before the bounds test, so a fractional value
       truncates and is never refused (Streams' `reader.read(v, {min: 1.5})` reads with min 1, and `{min: 2**60}`
       is a TypeError). */
    IDL_UNSIGNED_LONG_LONG_ENFORCE,
    /* `[Clamp] long long` — §3.2.4.9 clamps to the type's range and rounds to the nearest integer, ties to
       even, instead of the modulo. Blob's `slice(1.5)` starts at byte 2. */
    IDL_LONG_LONG_CLAMP,
    /* `unrestricted double` — Web IDL §3.2.8 unrestricted double: ToNumber only; NaN and the infinities are
       values (QueuingStrategy's `highWaterMark`, whose NaN refusal is the stream's own RangeError). */
    IDL_UNRESTRICTED_DOUBLE,
    /* `double` — Web IDL §3.2.7 double: ToNumber, then NaN or an infinity is a TypeError
       (`video.currentTime = NaN` throws). */
    IDL_DOUBLE,
    /* `float` — Web IDL §3.2.5 float: §3.2.7's refusals, plus a finite value outside the single-precision range
       is a TypeError, and the result is rounded to the nearest single-precision value, ties to even. The
       rounding is page-visible: PointerEvent's `float pressure` reads back 0.10000000149011612 for 0.1. */
    IDL_FLOAT,
    /* `double?` — §3.2.7 under §3.2.20 Nullable types. CookieInit's `DOMHighResTimeStamp? expires = null` needs
       it: ToNumber(null) is 0, the epoch, so IDL_DOUBLE would turn the IDL's own default into a real timestamp.
       Named residual: only a dictionary-member type. Not covered: a `double?` or `long long?` argument position,
       which the argument loop's closing DCHECK refuses as "a type this machine does not convert" (release
       stringifies it). Next diff: a numeric twin of the DOMString?/USVString? null rewrite in the argument
       loop, using idl_numeric_nullable_inner. Absence shows as that abort, naming the type, at the first call
       of a member declaring one. */
    IDL_DOUBLE_NULLABLE,
    /* `long long?` — Web IDL §3.2.4.7 long long under §3.2.20; separate from the row above because the inner
       conversion differs (a modulo, not §3.2.7's refusal). CookieInit's `long long? maxAge = null`: a 0 is
       `Max-Age: 0`, a deletion, while null is no Max-Age at all. IDL_DOUBLE_NULLABLE's residual covers it. */
    IDL_LONG_LONG_NULLABLE,
    /* A callback function type — Web IDL §3.2.19 Callback function types: a callable crosses as itself and
       anything else is a TypeError. An absent optional member is not refused. */
    IDL_CALLBACK,
    /* An enumeration — Web IDL §3.2.18 Enumeration types: ToString, then a value the IDL does not list is a
       TypeError, never silently the default (`new Blob([], {endings: "bogus"})` throws). The value list is
       part of the type and is declared beside the position (idl_arg_enum) or member (IdlDictMember::values).
       Over unknown external input it forks rather than crossing: the domain is the N listed strings plus the
       refusal, and the algorithms behind it tell them apart (Fetch's `credentials` decides whether cookies are
       sent). See idl_concolic_rule and idl_enum_fork. */
    IDL_ENUM,
    /* `E?` (`NavigationType? navigationType = null`): null and undefined are the IDL null; anything else is
       §3.2.18's conversion. IDL_ENUM would make the IDL's own null default a TypeError.
       Named residual: this row still crosses unknown input. Not covered: the N+2 worlds (the N members,
       §3.2.18's TypeError, and §3.2.20's null) an unknown `E?` stands for. Next diff: an N+2-outcome ask at
       the two sites where IDL_ENUM's fork stands. Absence shows as one dictionary placing a real enumeration
       string for an IDL_ENUM member and an unconverted unknown for an IDL_ENUM_NULLABLE member of the same
       unknown (`new NavigateEvent(t, {navigationType: cfg.k})`). */
    IDL_ENUM_NULLABLE,
    /* `(DOMString or Function)`, which is TimerHandler: a callable crosses as itself, anything else is a
       DOMString. Named for the rule, not the member. */
    IDL_STRING_UNLESS_CALLABLE,
    /* `boolean` — Web IDL §3.2.3 boolean: ToBoolean, which runs none of the page's code (the read before it
       may). Over unknown external input it forks at the branch seam rather than crossing, because a concolic
       is an Object, ToBoolean of it is true, and a boolean's only consumer is control flow. See
       idl_concolic_rule. */
    IDL_BOOLEAN,
    /* `object?` — Web IDL §3.2.13 object under §3.2.20: undefined and null become the IDL null, an Object
       crosses as itself, anything else is a TypeError (Console's `dir(x, 5)` throws). Reads no member. */
    IDL_OBJECT_NULLABLE,
    /* A `boolean` dictionary member with no default. Web IDL §3.2.17 Dictionary types leaves an absent member
       unset, so "absent" and "false" are two states; DOM's `observe` branches on which (`{attributeFilter: []}`
       succeeds, `{attributes: false, attributeFilter: []}` is a TypeError). MutationObserverInit's four
       defaultless booleans use this; its `= false` members stay IDL_BOOLEAN. A present value converts exactly
       as IDL_BOOLEAN, so unknown input forks the same way. */
    IDL_BOOLEAN_NO_DEFAULT,
    /* `sequence<DOMString>` — Web IDL §3.2.21 Sequences — sequence<T>. The iterator protocol is the page's code
       at every step, so the conversion parks on the element it is on, at an argument position or a dictionary
       member (DOM's `attributeFilter`). */
    IDL_SEQUENCE_DOMSTRING,
    /* `sequence<E>`, E an enumeration (Web Cryptography's `sequence<KeyUsage> keyUsages`). Each element's
       ToString is a rest point, and the membership test runs before the next pull — Web IDL §3.2.21.1 Creating
       a sequence from an iterable converts inside the repeat loop, so a bogus element throws before the next is
       pulled. The value list is declared as for IDL_ENUM. */
    IDL_SEQUENCE_ENUM,
    /* `sequence<double>` (Intersection Observer's `threshold`, through the union below). The pull and each
       element's ToNumber are both the page's code and both rest points. §3.2.7 refuses a non-finite element
       with a TypeError; the algorithm's own [0, 1] RangeError stays in the algorithm. */
    IDL_SEQUENCE_DOUBLE,
    /* `(DOMString or sequence<DOMString>)` — Web IDL §3.2.25 Union types. The arm is chosen by step 11.2's
       `? GetMethod(V, %Symbol.iterator%)`, which is the page's code, so the position parks there and the
       resolved arm is a resume point (`uni_phase`). An Object with a callable @@iterator takes the sequence;
       everything else, including an Object without one, null and numbers, takes step 15's string arm
       (`db.transaction({})` is the store name "[object Object]"). A non-callable, non-nullish @@iterator is a
       TypeError. Elements are DOMStrings, without §3.2.12's replacement. IndexedDB's `transaction` declares it. */
    IDL_DOMSTRING_OR_SEQUENCE,
    /* The same union, nullable: §3.2.25 step 2 makes null and undefined the IDL null before anything is read
       (IndexedDB's `keyPath: null` means no key path, not the string "null"). A dictionary member there. */
    IDL_DOMSTRING_OR_SEQUENCE_NULLABLE,
    /* `(double or sequence<double>)` — §3.2.25 with the same step 11.2 read. A callable @@iterator takes the
       sequence; everything else is ToNumber'd by step 13.1 or 17 (`{threshold: null}` is 0, `{threshold: "x"}`
       is NaN and a TypeError under §3.2.7). The arm resolver is shared with the string union
       (idl_union_seq_arm), so step 11.2 has one implementation. */
    IDL_DOUBLE_OR_SEQUENCE,
    /* `sequence<unsigned long>` — §3.2.21 with Web IDL §3.2.4.6 unsigned long as the element conversion:
       ToNumber (a rest point per element) and the modulo, so it refuses nothing (`[Infinity]` is 0, `[-1]` is
       4294967295), unlike IDL_SEQUENCE_DOUBLE. Reached through the union below. */
    IDL_SEQUENCE_UNSIGNED_LONG,
    /* `(unsigned long or sequence<unsigned long>)` — §3.2.25, as CSS Fonts' `CSSFontFeatureValuesMap.set`
       declares. Same step 11.2 arm resolver; the non-sequence arm is `unsigned long`, so `set("k", null)` and
       `set("k", Infinity)` are 0 and never throw. Declaring IDL_DOUBLE_OR_SEQUENCE here would throw on
       Infinity, and engine/idl_typename.mjs answers null for a union, so no audit sees that difference.
       Its concolic rule is the sibling's default CROSSES: the resolution site asks no fork, and the body stores
       the unknown, matching the spec's "a single unsigned long value is treated as a sequence of a single
       value". */
    IDL_UNSIGNED_LONG_OR_SEQUENCE,
    /* `sequence<I>`, I an interface (HTML's `GetHTMLOptions.shadowRoots`). The protocol parks per element; the
       element conversion is Web IDL §3.2.15 Interface types' brand test, which runs no page code. The interface
       is named by idl_iface_brand / idl_iface_narrow, as for IDL_INTERFACE. */
    IDL_SEQUENCE_INTERFACE,
    /* `FrozenArray<I>?` / `sequence<I>?`, I an interface. Web IDL §3.2.27 Frozen arrays — FrozenArray<T> converts
       from the sequence, so this is the row above under §3.2.20's null rule; the null clears WAI-ARIA's
       `ariaLabelledByElements`-shaped members. */
    IDL_SEQUENCE_INTERFACE_NULLABLE,
    /* `sequence<object>` — the element conversion is §3.2.13 object (an Object crosses, anything else is a
       TypeError), decided between pulls. HTML's `StructuredSerializeOptions.transfer`, taken by
       structuredClone and both postMessage forms, is a declared type so the walk is the iterator protocol and
       parks, rather than a C length-and-index read of a value that may be a Proxy. */
    IDL_SEQUENCE_OBJECT,
    /* `sequence<D>`, D a dictionary, with no union over it: every element runs §3.2.17's member walk, and an
       element that is not an Object, undefined or null is §3.2.17 step 1's TypeError (null and undefined give
       the all-defaults dictionary). Web Cryptography's JsonWebKey `sequence<RsaOtherPrimesInfo> oth` is the
       first. The element dictionary is IdlDictMember::dict. */
    IDL_SEQUENCE_DICT,
    /* `sequence<(DOMString or D)>`, D a dictionary (HTML's SanitizerConfig `elements`, whose entries' own
       `attributes` is the same shape again). The conversion is a stack of cursors, never C recursion: every
       pull, `done`/`value` read and member [[Get]] is the page's code and parks at any depth. The depth is a
       property of the declared type tree, so it is computed at declaration (idl_members_depth) and page data
       cannot make it deeper. The dictionary arm is IdlDictMember::dict.
       The iterable itself crosses unknown input (§3.2.21 names no arm; what an unknown iterable lacks is a
       length). Each element's union is forked like an argument position's, inside the element conversion
       (idl_conv_seq_run). */
    IDL_SEQUENCE_STRING_OR_DICT,
    /* `(DOMString or D)`, D a dictionary — §3.2.25 over HTML Sanitizer's `allowElement` and its siblings. The
       algorithm's order is observable: null and undefined take the dictionary (step 4, then a missing
       `required` member throws, so `allowElement(null)` is a TypeError), any Object takes it (step 11.4, a
       function included), and everything else takes step 15's string arm. Unknown input forks at both
       resolution sites, the argument position and the element of IDL_SEQUENCE_STRING_OR_DICT.
       Named residual: a `(D or E)` union whose string arm is an enumeration, such as the Sanitizer
       constructor's `(SanitizerConfig or SanitizerPresets)`, is not covered; this row converts that arm as a
       DOMString, which crosses an unknown where §3.2.18 forks N+1 ways. Next diff: an IDL_ENUM_OR_DICT row
       answering IDL_CONCOLIC_FORKS, with a per-position record of the resolved arm that survives a park, since
       the enumeration arm parks and re-asking the arm fork on resume trips `fork_ask_key`. Absence shows as
       the Sanitizer constructor reached carrying an unknown preset that no body can fork. */
    IDL_STRING_OR_DICT,
    /* The position where two overloads of different length split — Web IDL §3.6 Overload resolution
       algorithm, not a union. HTML's Window `postMessage(any, USVString targetOrigin, optional
       sequence<object> transfer = [])` against `postMessage(any, optional WindowPostMessageOptions = {})`.
       Steps 3-4 decide by argument count first: a call passing a third argument keeps only the longer entry,
       so this position is a required USVString there (`postMessage(m, {}, [])` stringifies the object). Where
       both entries stand, step 12 decides: undefined (12.2) and null (12.3) take the dictionary, any Object
       takes it (12.11), and everything else is step 12.15's USVString (`postMessage(m, 123)` is target origin
       "123"). Optionality is read from the surviving entry (step 15.3). Unknown input forks at that arity. */
    IDL_USVSTRING_OR_DICT,
    /* A §3.6 length split whose two entries never coexist at one arity, so no value is ever consulted: CSSOM
       View's `scroll(optional ScrollToOptions options = {})` against `scroll(unrestricted double x,
       unrestricted double y)`. Steps 3-4 leave one entry at every arity, so `el.scrollTo(0)` is the dictionary
       entry and a TypeError from §3.2.17 step 1. Its concolic rule is UNASKED: by the time it is consulted a
       longer-arity call has already been rewritten to the number, and a dictionary asks the value nothing. */
    IDL_UNRESTRICTED_DOUBLE_OR_DICT,
    /* The same never-coexisting split with a USVString longer arm: Cookie Store's `set(USVString name,
       USVString value)` against `set(CookieInit options)`. It is not IDL_USVSTRING_OR_DICT: every argument of
       both entries is required, so step 4 removes one entry at every arity and step 12 never runs —
       `cookieStore.set("x")` is the dictionary entry and a TypeError from §3.2.17 step 1, not a cookie name.
       Its concolic rule is UNASKED for the row above's reason. */
    IDL_USVSTRING_OR_DICT_BY_ARITY,
    /* The same split with the dictionary on the longer entry: Web Locks' `request(DOMString name,
       LockGrantedCallback callback)` against `request(DOMString name, LockOptions options, LockGrantedCallback
       callback)`. At arity 2 this position is the callback; at arity 3 it is the dictionary. The direction
       matters because idl_split_longer_type answers the longer entry's type, which here is the dictionary.
       Its concolic rule is UNASKED, as for IDL_UNRESTRICTED_DOUBLE_OR_DICT. */
    IDL_CALLBACK_OR_DICT,
    /* A §3.6 split where neither entry is longer: MessagePort's `postMessage(any, sequence<object> transfer)`
       against `postMessage(any, optional StructuredSerializeOptions options = {})`. Step 4 removes neither, so
       step 12 decides and reads the page's value (a rest point): undefined (12.2) and null (12.3) take the
       dictionary; an Object with a callable @@iterator takes the sequence (12.10, the same GetMethod read and
       resolver as §3.2.25 step 11.2, whose method step 14 reuses); any other Object takes the dictionary
       (12.11); everything else is step 12.20's TypeError (`port.postMessage(m, "x")` throws).
       Unknown input forks three ways: the two entries and the TypeError. */
    IDL_SEQUENCE_OBJECT_OR_DICT,
    /* `sequence<USVString>` — §3.2.21 with §3.2.12 elements, so an unpaired surrogate in an element becomes
       U+FFFD. First used as the sequence arm of the record value below. */
    IDL_SEQUENCE_USVSTRING,
    /* `record<USVString, (USVString or sequence<USVString>)>` at a dictionary member — Web IDL §3.2.23 Records —
       record<K, V> (File System Access's `FilePickerAcceptType.accept`). The conversion is core/idl_iter.c's
       RecordCursor, the same one Headers and URLSearchParams drive; the member loop pushes a frame holding it.
       It must be a member type, not a walk in the algorithm: §3.2.17 converts members in order, so the
       record's [[OwnPropertyKeys]] and reads are owed before the next member is read. The value's arm uses
       idl_union_seq_arm, the one implementation of §3.2.25 step 11.2. */
    IDL_RECORD_USVSTRING_STRING_OR_SEQUENCE,
    /* A dictionary — §3.2.17: each declared member is read in order and converted by its own type, through this
       same machine; every read and coercion is a request. The body receives an engine-built object carrying the
       converted members, read with an ordinary get. Members are declared with idl_method_id_dict. */
    IDL_DICT,
    /* `D?` — §3.2.17 under §3.2.20: null and undefined are the IDL null, where a plain `D` admits them and
       answers the all-defaults dictionary. Intersection Observer's `required DOMRectInit? rootBounds`: `{}` and
       `{rootBounds: undefined}` are §3.2.17 step 4.1.6's TypeError, `{rootBounds: null}` is null. The
       dictionary is IdlDictMember::dict.
       A dictionary-member type only: an argument position declaring it reaches the "type this machine does not
       convert" refusal, and would also need idl_type_is_dictionary's answer for §3.6's omitted-dictionary rule. */
    IDL_DICT_NULLABLE,
    /* `(AddEventListenerOptions or boolean)` — §3.2.25 in step order: null and undefined take the dictionary
       (step 4), any other Object takes it (step 11.4), and only what survives is the boolean (step 12 or 18).
       So an omitted argument, which `optional … = {}` hands this position as undefined, is the empty
       dictionary, never `false`. DOM's flatten options then reads `capture` from either arm. */
    IDL_DICT_OR_BOOL_FIRST,
    /* `(boolean or ScrollIntoViewOptions)` — the same two arms and test, but the boolean is placed as itself
       because CSSOM View's `scrollIntoView` step 6 reads it directly (`false` sets block to "end"). Here step 4
       is observable: `scrollIntoView()` and `scrollIntoView(null)` take the dictionary and stay at "start". The
       body tells the arms apart with `JS_IsBool`, which is §3.2.25's own output. Unknown input forks. */
    IDL_BOOL_OR_DICT,
    /* `(BufferSource or D)`, D a dictionary — Web Cryptography's `importKey` `keyData`, the only argument of
       this shape. §3.2.25: null and undefined take the dictionary (step 4); an ArrayBuffer (6), DataView (8)
       or typed array (9) takes the buffer arm; a SharedArrayBuffer matches step 7, places nothing (the union
       names neither SharedArrayBuffer nor object) and so reaches step 11.4's dictionary; any other Object takes
       the dictionary; any other primitive is step 20's TypeError. A row, not a body test, because the arms
       differ in what the conversion performs: the dictionary arm runs §3.2.17's walk, which parks.
       Unknown input forks three ways (see idl_concolic_rule). */
    IDL_BUFFERSOURCE_OR_DICT,
    /* `(I or DOMString)`, I an interface — DOM's "node or text" members (`el.append('hi')`): a value
       implementing I crosses as itself, anything else is a DOMString. The class is declared beside the type. */
    IDL_STRING_UNLESS_IFACE,
    /* `(double or I)`, I an interface — CSS Typed OM's `CSSNumberish`. §3.2.25 step 5.1 takes a value
       implementing I; this union names no other Object arm, so everything else reaches step 13.1 or 17's
       restricted `double` (`equals(null)` compares against 0, `equals(undefined)` is a TypeError).
       Unknown input crosses (idl_concolic_rule's default): a concolic implements no interface, so the numeric
       arm is its arm, and the numeric boundary passes it through. The brand is declared as for
       IDL_STRING_UNLESS_IFACE (idl_iface_brand, or idl_arg_iface for an interface no class names). */
    IDL_DOUBLE_UNLESS_IFACE,
    /* `(object or DOMString)` — Web Cryptography's AlgorithmIdentifier: any Object crosses as itself, and
       everything else, null and undefined included, is the DOMString arm. Not IDL_STRING_OR_DICT: this union
       names no dictionary, so `digest(null, b)` normalizes the algorithm "null" and rejects with
       "NotSupportedError". The algorithm's own dictionary conversion runs later, inside the member, at the step
       the standard numbers it, which keeps a throwing `name` getter a rejection. */
    IDL_STRING_UNLESS_OBJECT,
    /* `BodyInit?` — Fetch's `(ReadableStream or Blob or BufferSource or FormData or URLSearchParams or
       USVString)?`: null and undefined are the IDL null, a buffer source or a value of one of the named
       interfaces (each tested by the component that owns it) crosses as itself, everything else is the
       USVString arm. Fetch's extract a body reads the arm back off the value. */
    IDL_BODYINIT_NULLABLE,
    /* `sequence<BlobPart>` — §3.2.21 with `(BufferSource or Blob or USVString)` elements. A declared type so
       the walk runs in argument order, before later arguments convert. */
    IDL_SEQUENCE_BLOBPART,
    /* `(CSSOMString or BufferSource)` — CSS Font Loading's FontFace `source`. §3.2.25 steps 6, 8 and 9 take the
       buffer arm, which converts through Web IDL §3.2.26 Buffer source types (idl_buffer_source_refuse, as for
       IDL_BUFFERSOURCE); any other value, a plain Object included, passes step 11 (which names nothing this
       union has) to step 15's string arm. CSSOMString is DOMString here, as core/css/css_serialize.h states.
       Unknown input crosses: the buffer arm tests an internal slot a concolic does not have, so the string arm
       is its arm. */
    IDL_STRING_OR_BUFFERSOURCE,
    /* `BufferSource` — Web IDL §4.2 BufferSource, converted by §3.2.26: an ArrayBuffer, typed array or DataView
       crosses as itself, anything else is a TypeError. A shared or resizable buffer is refused
       (idl_buffer_source_refuse). */
    IDL_BUFFERSOURCE,
    /* `ArrayBufferView` — Web IDL §4.1 ArrayBufferView, converted by §3.2.26, with the same two refusals. Web
       Cryptography's `getRandomValues` needs it: an ArrayBuffer is a TypeError at the conversion, while a
       Float64Array reaches the algorithm's TypeMismatchError. Refusing resizable buffers here keeps a
       length-tracking view out of every position that did not ask for one. */
    IDL_ARRAYBUFFERVIEW,
    /* One of §3.2.26's typed arrays. The conversion tests [[TypedArrayName]] against T, then the
       [AllowShared] and [AllowResizable] refusals (Web IDL §3.3.2 [AllowShared], §3.3.1 [AllowResizable]).
       T and the two flags are parameters of the position, declared with idl_typed_array, rather than twelve
       rows; the flags are independent, so they are two flags rather than a product of rows. Encoding's
       `encodeInto(USVString, [AllowShared] Uint8Array destination)` is the first. */
    IDL_TYPED_ARRAY,
    /* An interface type — §3.2.15: a platform object implementing the interface crosses as itself and anything
       else is a TypeError, before the algorithm's step 1 (`walker.currentNode = null` throws). The class is
       declared with idl_iface_brand. */
    IDL_INTERFACE,
    /* `I?`, I an interface — §3.2.15 under §3.2.20: null and undefined are the IDL null, which is what makes
       `el.ariaActiveDescendantElement = null` a clear. The class is named as for IDL_INTERFACE. */
    IDL_INTERFACE_NULLABLE,
    /* A nullable callback interface — Web IDL §3.2.16 Callback interface types (`NodeFilter? filter`): any
       object is accepted (its operation is read off it by name), so `{acceptNode(){}}` is valid; only a
       primitive is a TypeError, and null and undefined are the IDL null. */
    IDL_CALLBACK_INTERFACE_NULLABLE,
    /* `(File or USVString or FormData)?` — HTML's `setFormValue`: null and undefined are the IDL null, a File
       or FormData crosses as itself, everything else is the USVString arm. A plain Blob is not an arm, so it
       stringifies. */
    IDL_FORMVALUE_NULLABLE,
    /* `(unsigned long or ImageDataArray)` — §3.6's distinguishing argument index for HTML's two ImageData
       constructors, `(unsigned long sw, unsigned long sh, optional ImageDataSettings settings = {})` and
       `(ImageDataArray data, unsigned long sw, optional unsigned long sh, optional ImageDataSettings
       settings = {})`. The entries differ at index 0 but the shorter ends at index 2, so the distinguishing
       index and the length split are two positions (see idl_overload_distinguishing_at).
       The arity decides first: at arity 4 only the longer entry stands, step 12 never runs, and this position
       converts by §3.2.25's typed-array clause and §3.2.26 with both buffer refusals. Where both entries stand,
       step 12.7 takes the longer entry for a matching typed array and step 12.16's numeric fallback takes the
       shorter for everything else, so step 12.20's TypeError is unreachable. Outcome 0 is the shorter entry. */
    IDL_ULONG_OR_IMAGE_DATA_ARRAY,
    /* A position behind the distinguishing index whose type differs per entry — §3.6 step 15.2's "the type
       at index i in the type list of the remaining entry" (ImageData's index 2: `ImageDataSettings` against
       `unsigned long sh`). It reads the entry settled at the distinguishing index and tests nothing; a union
       would give different answers (`new ImageData(2, 2, 5)` is a TypeError under §3.6). It must be in
       idl_type_is_dictionary, because the omitted-optional guard runs before any split resolves. */
    IDL_ULONG_OR_DICT_BY_ENTRY,
} IdlArgType;

/* What a declared type's conversion does to unknown external input, stated once: the argument conversion's
   pass-through and the assert over §3.2.25's arm block both read idl_concolic_rule, so they cannot disagree. */
typedef enum {
    /* Nothing is coerced, so unknown input is already what the body receives: `any` (including every position
       past a non-variadic member's declared arity), a dictionary (a bag of member reads, each a request
       yielding another unknown), and an interface brand (whose only answer for a non-platform object is a
       TypeError). */
    IDL_CONCOLIC_UNASKED = 0,
    /* The conversion coerces (ToString, ToNumber). Opacity must survive the coercion, so the value crosses as
       itself and the body asks it for what it needs (concolic_shape_c for bytes, the attribute taint shadow
       for a value parked in the DOM), as JSON.stringify yields an opaque field rather than a placeholder. */
    IDL_CONCOLIC_CROSSES,
    /* The conversion's answer over unknown input is a set of feasible worlds the member's algorithm tells
       apart, so both run. A type answers this only if the site that resolves it asks the fork; the two assert
       against each other. Two shapes reach it, at two seams:
         - a union or overload arm that tests the value and whose arms differ in what the conversion performs
           (a dictionary arm runs §3.2.17's member walk; a string arm does not), so no single placed value
           stands for both. Asked at the outcome seam, step_fork_run, numbered by the site. A union whose arms
           all place the value itself (`(DOMString or Function)`, `(Node or DOMString)`, `(object or
           DOMString)`) loses nothing by crossing and stays at CROSSES.
         - a conversion whose own domain is finite: §3.2.3 boolean, asked at the branch seam
           (step_tobool_run) so `if (cfg.on)` and a member taking `cfg.on` are one gate, and §3.2.18
           enumerations, asked at the outcome seam through idl_enum_fork. */
    IDL_CONCOLIC_FORKS,
} IdlConcolicRule;

static inline IdlConcolicRule idl_concolic_rule(IdlArgType t)
{
    switch (t) {
    case IDL_ANY:
    case IDL_DICT:
    /* §3.2.20's null test reads no property and §3.2.17 is a bag of member reads, as for IDL_DICT. */
    case IDL_DICT_NULLABLE:
    /* The never-coexisting §3.6 splits: by the time the rule is asked the argument count has rewritten the
       position to one entry's own type, so the only value the row describes is a dictionary or a callable. */
    case IDL_UNRESTRICTED_DOUBLE_OR_DICT:
    case IDL_USVSTRING_OR_DICT_BY_ARITY:
    case IDL_CALLBACK_OR_DICT:
    /* Resolved from the entry the distinguishing index already settled, so this row tests nothing and a fork
       here would be a second ask with no question behind it. */
    case IDL_ULONG_OR_DICT_BY_ENTRY:
    /* A record is a bag of reads ([[OwnPropertyKeys]], each key's descriptor, each Get), each a request
       yielding another unknown. The unknown-key-set fork belongs to step_ownkeys_run (`keys_pred`,
       `keys_probe`), which asks it once for every consumer; declaring FORKS here would ask it twice. */
    case IDL_RECORD_USVSTRING_STRING_OR_SEQUENCE:
    case IDL_INTERFACE:
        return IDL_CONCOLIC_UNASKED;
    /* `(AddEventListenerOptions or boolean)`: the dictionary arm makes DOM's flatten more options read `once`,
       `passive` and `signal`, the boolean arm leaves them at their defaults (a null `passive` is what makes a
       wheel listener on a Window passive by default). */
    case IDL_DICT_OR_BOOL_FIRST:
    /* `(boolean or ScrollIntoViewOptions)`: the boolean `false` scrolls to "end" where the dictionary stays at
       "start". */
    case IDL_BOOL_OR_DICT:
    /* `(DOMString or D)`: step 11 sends every Object, and so every concolic, down the dictionary arm. A
       preset name and a configuration are different algorithms (HTML Sanitizer), as are a string and an
       options bag for `createElement`. Outcome 0 is the dictionary arm (what a no-policy run takes, since a
       concolic is an Object); the string world is what the fork adds. Real null and undefined are not part of
       the fork; they take step 4's dictionary arm. */
    case IDL_STRING_OR_DICT:
    /* The §3.6 split at the arity where both entries stand: step 12.11 names the dictionary entry for any
       Object and step 12.15 the USVString entry otherwise, and the entry decides which conversion runs. At the
       longer arity steps 3-4 have already rewritten the position (idl_split_longer_type). Outcome 0 is the
       dictionary entry. */
    case IDL_USVSTRING_OR_DICT:
    /* The same-length §3.6 split: outcomes are (0) the dictionary entry, (1) the `sequence<object>` entry and
       (2) step 12.20's TypeError, which a two-armed fork would drop because a concolic is an Object. */
    case IDL_SEQUENCE_OBJECT_OR_DICT:
    /* `(BufferSource or D)`: outcomes are (0) the dictionary arm, (1) the buffer-source arm and (2) step 20's
       TypeError, reached by a real primitive since this union names no string, numeric, boolean or bigint
       type. Outcome 1 is a named crash at the site: an unknown has no bytes for §3.2.26 to copy. */
    case IDL_BUFFERSOURCE_OR_DICT:
    /* §3.2.3 boolean, at both boundaries (an argument and a §3.2.17 member), through the branch seam
       step_tobool_run, so `if (cfg.on)`, a member taking `cfg.on` and `{on: cfg.on}` file one constraint
       entry. A crossed boolean would answer `true` in every body's JS_ToBool. IDL_BOOLEAN_NO_DEFAULT differs
       only in what an absent member means, not in what §3.2.3 does with a present unknown. */
    case IDL_BOOLEAN:
    case IDL_BOOLEAN_NO_DEFAULT:
    /* §3.2.18: N+1 completions, the N declared values as the page can tell them apart plus the one refusal
       (non-member strings are one world, not many). Outcome 0 is the first value the declaration lists, an
       ordinary completion rather than the throw. Both boundaries ask it at the outcome seam through
       idl_enum_fork. IDL_ENUM_NULLABLE has one world more and is a residual at its row. */
    case IDL_ENUM:
    /* §3.6's surviving entry at ImageData's distinguishing index: a typed array or a number. Two outcomes,
       since the numeric fallback always names the shorter entry; one entry allocates transparent black from
       `sw`/`sh`, the other takes the page's buffer. */
    case IDL_ULONG_OR_IMAGE_DATA_ARRAY:
        return IDL_CONCOLIC_FORKS;
    default:
        return IDL_CONCOLIC_CROSSES;
    }
}

/* Which declared types need §3.2.15's `I` stated — "If V implements I, then return …; throw a TypeError".
   One predicate read by the conversion arms, idl_arg_iface's position check and the seal's sweep, so a new
   branding type is learned by all of them at once. The nullable rows are here because the declaration, not
   the null rule, is what must name the interface. */
static inline bool idl_type_brands_interface(IdlArgType t)
{
    switch (t) {
    case IDL_INTERFACE:
    case IDL_INTERFACE_NULLABLE:
    case IDL_SEQUENCE_INTERFACE:
    case IDL_SEQUENCE_INTERFACE_NULLABLE:
    /* The union's arm is the brand test, so without a brand no arm can be chosen. */
    case IDL_STRING_UNLESS_IFACE:
    /* Likewise: without a brand every value, the CSSNumericValue included, would take the numeric arm. */
    case IDL_DOUBLE_UNLESS_IFACE:
        return true;
    default:
        return false;
    }
}

/* Which declared types need §3.2.18's value list `E` stated. Read by idl_arg_enum's position check and the
   seal's sweeps over every argument position and dictionary member, never by a conversion: a conversion has
   already resolved one type and asks for it by name, because `E`, `E?` and `sequence<E>` are three different
   conversions. This answers the declaration-time question only. */
static inline bool idl_type_admits_enumeration(IdlArgType t)
{
    switch (t) {
    case IDL_ENUM:
    case IDL_ENUM_NULLABLE:
    case IDL_SEQUENCE_ENUM:
        return true;
    default:
        return false;
    }
}

/* Does this member type push a frame that names no dictionary? A record pushes one frame (its keys come from
   the page's object, so there is no declared member list) and nothing under it, because its value type pushes
   nothing. It is a second predicate because the seal pairs idl_type_pushes_level with IdlDictMember::dict in
   both directions; idl_members_depth counts both. A record whose value pushes would need a row here. */
static inline bool idl_type_pushes_record(IdlArgType t)
{
    return t == IDL_RECORD_USVSTRING_STRING_OR_SEQUENCE;
}

/* Which member types push a §3.2.17 level, each naming its dictionary in IdlDictMember::dict. One predicate
   read by idl_members_depth (which sizes a host's IdlConvFrame block, asserted by idl_dict_walk_start) and by
   the member loop that pushes, so a type cannot be pushed against a budget that did not count it. The count
   walks the declared type tree, which is finite, so page data cannot deepen it. The seal requires a type here
   to name a dictionary and a member naming one to have a type here. */
static inline bool idl_type_pushes_level(IdlArgType t)
{
    switch (t) {
    case IDL_DICT:
    case IDL_DICT_NULLABLE:
    case IDL_SEQUENCE_STRING_OR_DICT:
    case IDL_SEQUENCE_DICT:
        return true;
    default:
        return false;
    }
}

/* §3.2.17 step 4.1.5's default value: a member whose IDL writes `= …` exists on the converted dictionary even
   when the page wrote nothing (HTML Sanitizer canonicalization asserts SanitizerElementNamespace's namespace
   exists for that reason). Declaring it means the conversion places it and no reader invents it. Only the
   forms the platform declares are rows. */
typedef enum {
    IDL_DEFAULT_NONE = 0,   /* the IDL writes no `= …`: an absent member does not exist */
    IDL_DEFAULT_NULL,       /* `= null` */
    IDL_DEFAULT_STRING,     /* `= "…"`, the string `dflt_str` holds */
    IDL_DEFAULT_ZERO,       /* `= 0` (IDBVersionChangeEventInit's `oldVersion`) */
    IDL_DEFAULT_FALSE,      /* `= false` (Console's `assert(optional boolean condition = false, …)`) */
    /* `= true` (CanvasRenderingContext2DSettings' `alpha`): ToBoolean of an absent member is the opposite. */
    IDL_DEFAULT_TRUE,
    /* `= 1` (Streams' `[EnforceRange] unsigned long long min = 1`): `read(view)` rejects a min of 0. */
    IDL_DEFAULT_ONE,
} IdlDictDefault;

struct IdlDictDecl;

/* A dictionary member as its IDL declares it.
   `required`: an absent required member is a TypeError, and for a dictionary `undefined` is absent; omitting
   the field gives optional.
   `values`: the NULL-terminated §3.2.18 list of a member whose type idl_type_admits_enumeration answers true
   for, and of no other; the seal asserts both directions over every member list.
   `level`: which dictionary of the inheritance chain declares the member, 0 for the least derived (the root),
   counting up to D itself. §3.2.17 step 3 reads dictionaries from least to most derived and step 4.1 sorts
   each one's members, so ascending level is the read order (`FilePropertyBag : BlobPropertyBag` reads
   endings, type, lastModified). Every member states its true level, or idl_dict_order_check passes only while
   the orders happen to coincide.
   `dflt`/`dflt_str`: §3.2.17 step 4.1.5's default value. */
typedef struct {
    const char *name;
    IdlArgType  type;
    bool        required;
    const char *const *values;
    uint8_t     level;
    /* The dictionary this member's conversion builds or walks: non-NULL exactly for the types
       idl_type_pushes_level answers true for, which the seal asserts. */
    const struct IdlDictDecl *dict;
    IdlDictDefault dflt;
    const char *dflt_str;
    /* This member's own §3.2.15 class, for a dictionary declaring more than one interface (NavigateEventInit's
       NavigationDestination, AbortSignal, FormData and Element). Zero means the declaration's idl_iface_brand
       states it, and the conversion asserts one of the two did. */
    JSClassID   iface;
    /* The narrowing a class cannot express, on the same member (`Element? sourceElement` against the one Node
       class). Taken together with `iface` by idl_member_iface, so a member stating its class does not inherit
       the declaration's narrowing. NULL means the class names the interface exactly. */
    bool      (*iface_narrow)(JSValueConst v);
    /* §3.2.15's `I` stated as a realm-aware predicate instead of a class — the dictionary counterpart of
       idl_arg_iface — for an interface many classes implement (`EventTarget`), whose instance is the realm's
       global (`Window`), or whose class is shared by construction (core/idl_indexed.c's indexed interfaces,
       branded by an own slot). A member states `I` exactly once, by class or by this, which the seal asserts;
       idl_member_implements reads both. `iface_name` is the IDL identifier the TypeError names, a static; NULL
       for a member that states its class, whose message uses idl_member_iface_subject. */
    bool      (*iface_is)(JSContext *ctx, JSValueConst v);
    const char *iface_name;
} IdlDictMember;

/* An initializer of IdlDictMember names its fields from `iface` on: a positional list reaching the tail would
   silently re-aim values when a field is added, and the adjacent function pointers would convert without a
   diagnostic. A positional list that stops before the tail is fine. */

/* A named dictionary: its members in §3.2.17 read order and its IDL identifier, used when it is reached as a
   member's nested type so a diagnostic can say which dictionary refused a value. A member's own dictionary
   argument is declared as the bare list (idl_method_id_dict). */
typedef struct IdlDictDecl {
    const char          *name;
    const IdlDictMember *members;
    int                  n;
} IdlDictDecl;

/* ---- Web IDL §3.2.17 Dictionary types, as an embeddable walk ---------------------------------------------
 *
 * One walk serves both entries to §3.2.17's ES-to-IDL conversion: a declared IDL_DICT argument converted at
 * the argument boundary, and an algorithm converting a value it holds (IndexedDB's getAll decides only at its
 * step 8 whether its `any` argument is an IDBGetAllOptions). Every member [[Get]] and every coercing member
 * conversion is the page's code, so a second copy would drift in required-member, default and coercion rules.
 *
 * Every rest point is a request (step_getprop_run, step_tostring_run, step_todouble_run, iter_cursor_run),
 * which parks and resumes at its own call site with the host's stage unmoved, so an embedder adds a field and
 * a re-entry, never a stage. Requests are issued through the host's JSStepHdr, passed as a parameter, so the
 * walk has no identity of its own for the driver to assert about. */

/* ---- One level of §3.2.17, and the stack of them ---------------------------------------------------------
 *
 * Step 4.1.4.1 converts a member by its declared type, which may be another dictionary or a sequence of them,
 * so the conversion is a stack of levels. A level is everything the member loop reads: which list, where in
 * it, and what is in flight on the current member. It is a level and not C recursion because every rest point
 * is the page's code and a park must be a return. All levels run one member loop.
 */
typedef struct {
    JSValue   src;      /* jsDict — step 4.1.3.1 reads from it; undefined or null is step 4.1.2's "no object" */
    JSValue   out;      /* step 2's idlDict, as the object this engine represents one by (owned) */
    const IdlDictMember *members;
    const JSAtom        *atoms;   /* their names, interned when the dictionary was declared */
    const char *name;   /* the dictionary's IDL identifier, for a diagnostic; NULL for an anonymous one */
    int       n;
    /* The declaration-wide §3.2.15 class and narrowing (idl_iface_brand / idl_iface_narrow). A member carrying
       its own IdlDictMember::iface overrides both. Zero and NULL for a level with no interface-typed member and
       for every pushed level, whose interface-typed members each name their own; the conversion asserts it. */
    JSClassID iface;
    bool    (*narrow)(JSValueConst v);
    int       mi;       /* the resume point: the member being read */
    /* 0 = read the member (step 4.1.3.1), 3 = decide whether it is present (step 4.1.4), 1 = convert it
       (step 4.1.4.1), 2 = place it. A pushed level returns to 2 with the converted dictionary. 3 runs between
       0 and 1 but is numbered last so recorded phases keep their meaning; it is its own phase because the
       presence question can fork, and the resume must not re-run the getter or release a parked conversion. */
    uint8_t   mphase;
    JSValue   mv;       /* the member's value between those phases (owned) */
    /* §3.2.21's cursor and the list it fills, for a sequence-typed member, and also for a sequence at an
       argument position: arguments convert strictly left to right, so the two are never in flight at once.
       Per level because an outer member's sequence and an inner level's can be. */
    IterCursor seq;
    JSValue    seq_list;
    uint32_t   seq_n;
    /* 0 = not started, 1 = pull the next element, 2 = convert the one just pulled. Not started is a phase,
       not a null list, because a zeroed JSValue is the integer 0 (JS_TAG_INT is 0), not JS_UNDEFINED. */
    uint8_t    seq_phase;
    /* §3.2.25's resolved arm for an @@iterator union member or argument; a resume point because the decision
       is `? GetMethod(V, %Symbol.iterator%)`, the page's code. */
    uint8_t    uni_phase;
} IdlDictLevel;

/* What a pushed frame converts. A dictionary frame's result is the member's value one level down; a sequence
   frame's result is one element, which joins the list before the next pull; a record frame's is one key and
   value pair. */
enum { IDL_FRAME_DICT = 0, IDL_FRAME_SEQUENCE, IDL_FRAME_RECORD };

/* One pushed level. For IDL_FRAME_DICT, `lvl` is the nested dictionary. For IDL_FRAME_SEQUENCE it is the
 * sequence's iterator plus the element being converted as a dictionary in `lvl`: §3.2.21.1 converts inside the
 * repeat loop, so the element's §3.2.17 is a level like any other and parks at any depth. */
typedef struct {
    IdlDictLevel lvl;       /* the dictionary this frame is converting — its own, or the element it stands on */
    IterCursor  cur;        /* SEQUENCE only: the sequence's iterator, over `src` */
    JSValue     src;        /* SEQUENCE only: the value being iterated (owned) */
    JSValue     list;       /* SEQUENCE only: the elements converted so far (owned) */
    const IdlDictDecl *d;   /* SEQUENCE only: the element type's dictionary arm */
    /* SEQUENCE only: the element's declared type, which decides whether a pulled element has an arm to
       resolve (`sequence<(DOMString or D)>`) or is §3.2.17 outright (`sequence<D>`). */
    IdlArgType  elem;
    /* RECORD only: §3.2.23 as core/idl_iter.c's RecordCursor, the same one Headers and URLSearchParams drive;
       every key list, descriptor and Get is a request, so the frame parks at the key it stands on. The value's
       own conversion uses `lvl.uni_phase` and `lvl.seq*`, so no second frame is pushed for it. */
    RecordCursor rec;
    uint32_t    n;          /* SEQUENCE: how many elements `list` holds. RECORD: how many PAIRS it holds */
    uint8_t     kind;       /* IDL_FRAME_DICT / IDL_FRAME_SEQUENCE / IDL_FRAME_RECORD */
    uint8_t     phase;      /* SEQUENCE and RECORD */
} IdlConvFrame;

/* §3.2.17 in flight: everything a park must carry that the host cannot re-derive.
 *
 * The frames are not stored here. A deep fork byte-copies the hosting state and re-takes only what its
 * `visit` names, so a pointer into that block would survive the copy aimed at the original; the host passes
 * the frames to every entry from its own layout instead. An inline array would cap how deeply declared types
 * may nest.
 *
 * `members`/`atoms`/`iface`/`narrow` are borrowed statics that outlive the walk. They are stated on the walk
 * because an algorithm's dictionary has no member declaration to read them from. */
typedef struct {
    /* Level zero: the dictionary the host asked for; the member loop runs on whichever level is on top. */
    IdlDictLevel lvl;
    uint8_t    conv_sp;   /* how many IdlConvFrame frames are live; 0 = level zero is the one in flight */
    uint8_t    started;   /* the walk has a `src` and an `out`; 0 = nothing in flight, so a resume may start it */
    /* The name of the fork this conversion is asking: step_fork_run's `op` for step 4.1.4's presence question
       over an unknown source, step_tobool_run's for a boolean member, step_fork_run's for an enumeration. It
       is a field, not a local, because the driver reads JSStepHdr::fork_op after the machine returned
       JS_STEP_FORK; it is not the header's `len_op`, which belongs to the length probe.
       One buffer serves all three because one fork is in flight at a time (step_fork_ask refuses a second
       ask), and the asks are sequential. JSStepHdr::fork_ask_key hashes the string, and each ask names its spec
       step and member or position, so an answer cannot be consumed at another ask's site. Scratch: re-composed
       from the declaration on every entry, so a byte-copied clone never reads stale contents. */
    char       ask[160];
} IdlDictWalk;

/* Intern a dictionary declaration's member names, once per runtime, and return them. An atom must stay live
   between a request and its answer, so names cannot be interned per read. Also runs §3.2.17's read-order check
   over the declaration, which is why an algorithm's dictionary goes through this rather than JS_NewAtom.
   Idempotent. Call it from the component's per-agent init. */
const JSAtom *idl_dict_declare(JSContext *ctx, const IdlDictDecl *d);

/* Begin §3.2.17's ES-to-IDL conversion over `src`. The caller has already performed step 1 (a value that is
 * not an Object, undefined or null is a TypeError there), and this asserts it. undefined and null run the same
 * member loop, every member reading as undefined (step 4.1.2), so the result carries every declared default.
 *
 * `frames`/`frames_cap` are the nested-conversion stack, at least idl_members_depth deep, which this asserts.
 * NULL/0 is right for a dictionary none of whose members pushes a frame (idl_type_pushes_level or
 * idl_type_pushes_record), which is nearly all of them.
 * Returns 0, or -1 with a throw live (the object could not be minted). */
int  idl_dict_walk_start(JSContext *ctx, IdlDictWalk *w, JSValueConst src,
                         const IdlDictMember *members, int n, const JSAtom *atoms, const char *name,
                         JSClassID iface, bool (*narrow)(JSValueConst v),
                         IdlConvFrame *frames, int frames_cap);

/* Drive the conversion one re-entry's worth. Returns >0 (the caller returns it: the walk is parked in a
   member's [[Get]] or coercion), 0 when every member is converted, or -1 with a throw live. `in` is the
   request answer and is consumed. */
int  idl_dict_walk_run(JSContext *ctx, JSStepHdr *hdr, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap,
                       JSValue in, JSValue **out_cb, int *out_argc);

/* Take step 5's idlDict, owned, and leave the walk empty so another may start. Asserts the walk finished: a
   half-read dictionary's absent members would be indistinguishable from members the page did not write. */
JSValue idl_dict_walk_take(JSContext *ctx, IdlDictWalk *w);

/* What the walk owns, for the hosting state's `visit` to chain into. Visits every frame, not only live ones:
   a popped frame holds JS_UNDEFINED and an unused one a zeroed integer, so neither takes a reference, and a
   loop bounded by `conv_sp` would drop a frame's value if the two ever disagreed. */
void idl_dict_walk_visit(JSContext *ctx, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap, JSStepVisit *v);

/* Release everything an abandoned walk holds and leave it empty, for a host whose teardown is not the
   `visit`-driven discharge. A host whose `visit` names the walk needs nothing here. Safe on an unstarted walk. */
void idl_dict_walk_clear(JSContext *ctx, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap);

/* Declare a member: the IDL types of its arguments and the body to run once they are converted. Returns the
   step id, which the caller caches; registration is separate from installation so that members installed on
   every wrapper do not mint a definition per object. A position past `nargs` is passed through unconverted
   (a variadic `any...` tail, or an unlisted optional argument). There is no ceiling on `nargs`: the
   declaration owns a copy of `types` (a caller may pass a stack array) and the per-call argument vector is
   sized from the same number. */
int  idl_method_id(JSContext *ctx, const IdlArgType *types, int nargs, IdlBody body, int magic);

/* §3.2.4.9's integer conversion over an already-computed ToNumber result: sign(x)·floor(|x|) modulo the
   type's width, folded into range if signed, or [Clamp]'s round-half-to-even. Public so a conversion outside
   this machine shares the arithmetic (a NodeFilter's `acceptNode` returns an `unsigned short`, so 65537
   accepts like 1). */
int64_t idl_integer_of(IdlArgType t, double x);

/* The number a converted numeric argument denotes, for a body that needs a real one; the numeric twin of
   concolic_name_cstr. A body must not call JS_ToFloat64 on its own numeric argument, because unknown input
   crosses the conversion as itself and reaches the body as the unknown, which owes C no real number.
   For an unknown, this answers §3.2.4.9's conversion run on that value's own example, through
   idl_integer_of; the value itself stays unknown. Returns 1 with `*out` written, or 0 when the unknown carries
   no example yet: there is no number to invent, and what that means differs per member, so the caller
   decides. */
int idl_number_of(JSContext *ctx, IdlArgType t, JSValueConst v, double *out);

/* Web IDL §3.2.4.8 unsigned long long as a magnitude rather than the int64_t bit pattern the modulo leaves
   (the half above 2**63 is what a page reaches by writing a negative). Public for the reason idl_integer_of is
   (File System's write algorithm reads its own dictionary members). */
double  idl_unsigned_long_long_of(double x);

/* §3.2.11's ByteString range over UTF-8 bytes: true when every code point is 0x00..0xFF. Public because
   Headers converts a record's keys itself, and the range is the type's rule. */
bool idl_is_bytestring(const char *utf8, size_t len);

/* idl_method_id for a member whose IDL tail is variadic and/or that takes an interface-or-string union.
   `variadic` applies the last declared type to every argument from there on, stated per member because
   assuming it converted addEventListener's callback to a string. `iface` is the class an object must be to
   cross an IDL_STRING_UNLESS_IFACE position as itself. */
int  idl_method_id_ext(JSContext *ctx, const IdlArgType *types, int nargs, bool variadic, JSClassID iface,
                       IdlBody body, int magic);

/* idl_method_id for a member taking an IDL_DICT argument: `members` lists the dictionary's members in Web
   IDL's read order. A member declares at most one dictionary argument (a second would need its own cursor),
   which is a DCHECK. There is no ceiling on `nmembers`. `members` must outlive the declaration (every caller
   passes a static), so the pool keeps the pointer. */
int  idl_method_id_dict(JSContext *ctx, const IdlArgType *types, int nargs,
                        const IdlDictMember *members, int nmembers, IdlBody body, int magic);

/* A member whose algorithm runs the page's code after its arguments are converted (HTML's
   customElements.define reads the static `observedAttributes` getter and converts it). A plain body cannot:
   JS_GetProperty there is a C activation hosting the page's loops. So the body is itself a step, with the
   converted arguments in place, the same return contract as a JSTrampStepDef step, and its own state, whose
   size the member declares, which is zeroed before the first entry and whose owned values its visit names.
   `presult` receives the member's answer. */
typedef int (*IdlStepBody)(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc);

/* The first stage that is the member's own. Stages 0 and 1 belong to the hosting machine (the argument-count
   check and the conversions, both rest points). A body rests on `hdr->stage`, not a private counter, because
   the driver asserts at do_step_step that a machine's stage is one its declaration names; a private byte is a
   resume point nothing can check. */
#define IDL_STEP_FIRST 2

/* Bases a member's own stage X-list at IDL_STEP_FIRST. JS_STEP_STAGE_ENUM numbers from zero, and the first two
   stages belong to the prologue, so every list is expanded as:

       enum { IDL_STEP_STAGE_BASE(QS_STAGES) QS_STAGES(JS_STEP_STAGE_ENUM) };

   `list` names the X-list so two members in one file declare distinct enumerators; C numbers the next
   enumerator as this one plus one. */
#define IDL_STEP_STAGE_BASE(list) list##_base = IDL_STEP_FIRST - 1,

/* A step member's declaration. `visit` is the one statement of what the state owns: the deep-fork clone takes
   a second reference to each field, the teardown (tramp_step_state_free_1, after idl_args_result has stated the
   completion) releases each, and idl_args_result folds them to check that `release` left them alone. There is
   no second list of owned values. */
typedef struct {
    IdlStepBody body;
    size_t      state_size;
    void      (*visit)(JSContext *ctx, void *state, JSStepVisit *v);
    /* What `visit` cannot name and holds no reference: a lexbor handle, a foreign C allocation, or a flag the
       algorithm took and must give back on every exit (HTML's custom element definition's "regardless of
       whether the above steps threw", HTML §4.10.22.4 "Constructing the entry list" step 8's give-back of the
       constructing-entry-list flag). It runs before the declaration is discharged, so it may read owned values.
       idl_args.c folds every declared slot's identity (tag, and payload where that is a pointer) on each side
       of the call and requires them equal: `release` may move reference counts elsewhere but must not free,
       null, replace or hand over a declared slot. NULL when there is nothing of that kind. The active custom
       element constructor map is given back by idl_active_ctor_owed instead, for its nesting order. */
    void      (*release)(JSContext *ctx, void *state);
    /* Which algorithm this member is and which step each stage rests at: the pool builds one JSTrampStepDef per
       member, prepends labels for its own two stages, and the driver's one check reads it. `steps` is indexed
       from IDL_STEP_FIRST and NULL-terminated; each label uses the standard's wording ("DOM §4.4 step 3") so a
       parked flow says where it is, the same way in every session. A stage names one spec step; a range only
       when the whole range is one O(1) engine action (quickjs-step.h's JSTrampStepDef::steps states the rule,
       and js_step_def_check refuses a label arguing from the page's code). Both or neither are declared. */
    const char *algorithm;
    const char *const *steps;
    /* Whether this member's own algorithm catches an abrupt request result. The pool's definition always
       declares JSTrampStepDef::catches_abrupt, because HTML §4.13.6's step 1.3.1 catches in the epilogue every
       member ends through. Zero: the epilogue handles it and the body never sees it. One: the body is re-entered
       with JS_EXCEPTION at the request's call site with the throw live (DOM §4.9's "run these steps while
       catching any exceptions", which lets createElement report a throwing constructor). Such a body must
       answer the abrupt at every request; re-issuing it re-asks forever, because the keyed read's cursor is
       reset by the abrupt delivery. */
    uint8_t     catches_abrupt;
    /* Why this member's state must not be forked right now, or NULL when it may be. Forwarded onto the pool's
       definition, so the fork asks the member through the one door it asks everything else (see
       JSTrampStepDef.unforkable). A reason names a missing capability, not a machine, so several declarers
       retire together when it is built; find them by grepping for `unforkable` initializers, both positional
       and designated, rather than from a list here. */
    const char *(*unforkable)(const void *state);
} IdlStepDecl;

/* Declare where the optional arguments start. Per §3.6, `undefined` passed for an optional argument with no
   default means the argument is missing (`new URL("aaa:b", undefined)` is a one-argument call). Applies to the
   member the last declaration made, because a declaration returns the runtime's step id, not this pool's
   index. `first_optional` indexes the member's own list; `nargs` means none, and the bound is asserted. A
   member that never calls this has only required arguments. */
void idl_optional_from(int first_optional);

/* §3.6's "if X is given": whether `values` holds an IDL value or "missing" at `index` (step 9's two kinds of
 * entry). `argc` does not answer it: a page passing `undefined` at an optional position with no default has
 * raised the count for a missing argument (step 15.4.2, `new Audio(undefined)`), and the count handed to a
 * body is extended over every defaulted or dictionary position behind the passed ones (step 16.1).
 *
 * Ask it of an optional position only (at or past idl_optional_from). A required position is always given,
 * and a position with a declared default is always given (no IdlDictDefault produces `undefined`).
 * `undefined` in the vector is the representation of "missing"; the machine asserts both directions, at the
 * placement and at the body boundary. This is `!JS_IsUndefined(argv[i])`, named so a site states the spec's
 * question and the representation has one place to change. */
bool idl_arg_given(int argc, JSValueConst *argv, int index);

/* Declare where the longer overload entry's optional arguments start, for a §3.6 split whose entries differ in
 * length. Step 15.3 reads optionality from the remaining entry, and the declaration's idl_optional_from is the
 * shorter entry's. CSSOM View's `scroll` needs it: position 0 is optional for the dictionary entry, while
 * position 1 is required in the entry surviving at arity 2, so `el.scrollTo(1, undefined)` converts
 * ToNumber(undefined) rather than reading a missing optional.
 *
 * Applies to the member the last declaration made. Every member declaring a length split must state it, which
 * idl_args_seal asserts. */
void idl_overload_split_optional_from(int longer_first_optional);

/* Declare a §3.6 length split whose two entries share their type at the split, which the type list cannot
 * state. Steps 3-4 remove an entry by argument count, turning on the last position the shorter entry declares;
 * step 15.2's type question only has a second answer where the lists differ. CSS Conditional Rules'
 * `supports(property, value)` and `supports(conditionText)` share a prefix and type: without this,
 * `CSS.supports("(width:1px)", undefined)` would read position 1 as missing instead of converting it.
 *
 * `shorter_last_position` is the shorter entry's last position, which is what `split_at` means for a typed
 * split too. -1 is a shorter entry with no arguments (HTML select's `remove()` against `remove(long index)`),
 * not a sentinel; whether a split exists is the separate `has_split` field, because one field answering both
 * questions breaks at -1. Applies to the member the last declaration made, before
 * idl_overload_split_optional_from. A member whose type list already names a split may not also state one. */
void idl_overload_length_split_at(int shorter_last_position);

/* Declare §3.6's distinguishing argument index `d`, where step 12 chooses the surviving entry. It is a
 * different number from `split_at` (where the shorter entry ends, which steps 3-4 remove by): Web IDL §2.5.8
 * Overloading only requires the types before `d` to agree. Window's `postMessage` differs and ends at index 1;
 * ImageData differs at 0 and ends at 2.
 *
 * It is read off the type list wherever a value-resolved split row states it, so a second statement could
 * disagree. This entry is for a split at a position its type list cannot name; none exists yet, so it asserts
 * the declaration loop already found one. */
void idl_overload_distinguishing_at(int d);

/* Declare §3.6's default value at a positional argument, the third state beside passed and missing, as
   IdlDictDefault is for a dictionary member. §3.6 places a declared default in two steps: 15.4.1 (the page
   reached the position and passed `undefined`; sibling 15.4.2 is the missing rule) and 16.1 (the page stopped
   short; sibling 16.2 appends "missing" only for a non-variadic argument). Step 11.4.1 is the third such
   clause, reachable only when step 8 sets `d` for more than one entry, which a length split never reaches.
   IndexedDB's `transaction(storeNames, optional IDBTransactionMode mode = "readonly")` needs it, so no body
   re-derives the IDL's default. The default is an IDL value and is placed, never coerced or enum-checked.
   Applies to the member the last declaration made; the position must be declared and already optional, both
   asserted. `dflt_str` must outlive the declaration. */
void idl_arg_default(int index, IdlDictDefault dflt, const char *dflt_str);

/* Declare the class an IDL_INTERFACE / IDL_STRING_UNLESS_IFACE position brands against. Applies to the member
   the last declaration made, and composes with every declaration form. */
void idl_iface_brand(JSClassID iface);

/* Narrow an IDL_INTERFACE position past what a class can express: every DOM node wrapper is one class, so a
   class says "a Node" but not "an HTMLElement" (`setValidity(flags, msg, anchor)` must refuse a
   non-HTML element). Runs after the class check, failing with the same TypeError. Applies to the member the
   last declaration made. This is the declaration-wide form; a dictionary member with its own interface uses
   IdlDictMember::iface_narrow. */
void idl_iface_narrow(bool (*is)(JSValueConst v));

/* Declare §3.2.15's `I` at one position, overriding the two declarations above there only. Needed for a
 * member with more than one interface in its argument list (MouseEvent's `initMouseEvent` takes a `Window?`
 * and an `EventTarget?`), and for an interface no class names (`EventTarget`, implemented by many classes;
 * `Window`, the realm's global or a WindowProxy).
 *
 * The predicate takes a JSContext because implementing an interface through a prototype chain is a per-realm
 * fact; the conversion passes the member's realm. `iface` is the IDL identifier the TypeError names, a static.
 * Applies to the member the last declaration made; the position must be declared with a type
 * idl_type_brands_interface accepts (asserted), and idl_args_seal asserts every branding position has a brand. */
void idl_arg_iface(int index, bool (*is)(JSContext *ctx, JSValueConst v), const char *iface);

/* Declare the interface this member's receiver must implement — Web IDL §3.7 Interfaces' implementation-check
 * an object, step 3: "If object does not implement interface, then throw a TypeError."
 *
 * Answered before any argument converts: §3.7.7 Operations' create an operation function checks the receiver
 * before §3.6 runs, so `Iface.prototype.member.call({}, {toString() {…}})` throws without running the page's
 * toString. A body-side test would run it first.
 *
 * The predicate is the whole test, unlike idl_iface_brand: a member declared on Element is reached on an
 * HTMLDivElement with a different class id, so the component's own `…_is` predicate is named. `iface` is the
 * IDL identifier the TypeError names, a static. Applies to the member the last declaration made. */
void idl_this_iface(bool (*is)(JSValueConst v), const char *iface);

/* Declare §3.2.18's `E` at one position, as the NULL-terminated array of the identifiers the IDL lists. The
 * conversion refuses a ToString result not in it with a TypeError (`db.transaction("s", "bogus")`).
 *
 * It is per position because one argument list may name two enumerations (Web Cryptography's `importKey`
 * takes a KeyFormat at 0 and a `sequence<KeyUsage>` at 4). There is no declaration-wide form.
 *
 * Applies to the member the last declaration made; the position must be declared with a type
 * idl_type_admits_enumeration accepts (asserted), and idl_args_seal asserts every such position has a list.
 * `values` must outlive the declaration; write it with IDL_ENUM_VALUES. */
void idl_arg_enum(int index, const char *const *values);

/* Define a §3.2.18 value list and supply its NULL terminator, so it cannot be left off. Both readers (the
 * positional conversion and an IdlDictMember's `values`) receive a pointer and scan for NULL; a missing
 * terminator is an out-of-bounds scan, which the compiler may turn into a hang. The terminator is bookkeeping,
 * not part of the enumeration.
 * The extent is left unwritten (`[]`): an `extern T x[N]` whose definition has more entries is truncated with
 * only a warning, and the entry dropped is the terminator. A list shared across translation units uses the
 * EXTERN form and is declared `extern const char *const name[];` in its header. check.h's DCHECK_SENTINEL is
 * for a list not declared by these macros. */
#define IDL_ENUM_VALUES(name, ...)        static const char *const name[] = { __VA_ARGS__, NULL }
#define IDL_ENUM_VALUES_EXTERN(name, ...)        const char *const name[] = { __VA_ARGS__, NULL }

/* Declare which of §3.2.26's typed arrays an IDL_TYPED_ARRAY position is, and which buffer extended attributes
   its IDL writes. The conversion cannot start without T (it tests [[TypedArrayName]] against T's name), and a
   member may declare several (Web Audio's `getFrequencyResponse` takes three Float32Arrays), so it is per
   position; `index` is into the member's own type list.
   `allow_shared` is §3.3.2 [AllowShared] and `allow_resizable` is §3.3.1 [AllowResizable], the conditions
   §3.2.26's refusals turn on; two flags because the attributes are independent. Applies to the member the
   last declaration made; idl_args_seal asserts both directions (an IDL_TYPED_ARRAY position states T, and a T
   is stated only at one). */
void idl_typed_array(int index, JSTypedArrayEnum kind, bool allow_shared, bool allow_resizable);

/* Declare that this member's tail is variadic (`T... name`): the last declared type applies to every argument
   from that position on. Applies to the member the last declaration made, and composes with every declaration
   form, including a step body (Console's `log(any... data)` and its siblings). */
void idl_variadic(void);

/* Declare that this member's IDL return type is a promise. §3.7.7's create an operation function wraps the
 * brand check, overload resolution, every argument conversion and the method steps, and "If op has a return
 * type that is a promise type, then return ! Call(%Promise.reject%, %Promise%, «E»)". So
 * `crypto.subtle.digest('SHA-256', {})` rejects rather than throws, and a declaration says so instead of a body
 * re-deriving its argument types. Applies to the member the last declaration made, and composes with every
 * declaration form. */
void idl_returns_promise(void);

int idl_method_id_step(JSContext *ctx, const IdlArgType *types, int nargs,
                       const IdlDictMember *members, int nmembers,
                       const IdlStepDecl *decl, int magic);

/* The magic this invocation was declared with, read off the header because a step body's signature is the
   shared step contract (innerHTML and outerHTML are one walk with two starting points). */
int idl_step_magic(const JSStepHdr *hdr);

/* Declare that this invocation entered the active custom element constructor map, so the machine gives the
 * entry back at its teardown. `ctor` is DOM §4.9 create an element step 5.1.1's `C`, borrowed; the machine
 * takes its own reference.
 *
 * Not done in the member's `release`: steps 5.1.5-5.1.6 must also run for a discarded flow parked in the
 * page's constructor, so only the teardown can run them, and they must unwind in nesting order with HTML
 * §4.13.5 "Upgrades" step 10's give-back (custom_elements_queue_unlock), which the teardown pays below
 * idl_args.c's `release` bracket. One per invocation, asserted. */
void idl_active_ctor_owed(JSContext *ctx, JSStepHdr *hdr, JSValueConst ctor);

/* Read a member of the dictionary the declaration built. An omitted `optional D options = {}` argument is not
   there at all, so a plain property get would be a get on `undefined`; here an absent dictionary has every
   member absent. Nothing of the page's is on the object, so no page code runs. */
JSValue idl_dict_get(JSContext *ctx, JSValueConst dict, const char *name);
/* Read a boolean member. A macro passing the caller's address, because one assert line is reached from every
   dictionary and the member name alone (`bubbles`, `capture`) does not say which read it was.
   The member loop has already performed §3.2.3's fork and step 4.1.4's presence decision, so the value is a
   real truth value or absent. An unknown here means the object never went through §3.2.17 or the member is
   not declared a boolean, and the refusal says which. */
bool    idl_dict_bool_at(JSContext *ctx, JSValueConst dict, const char *name,
                         const char *file, int line);
#define idl_dict_bool(ctx, dict, name) idl_dict_bool_at((ctx), (dict), (name), __FILE__, __LINE__)

/* Assert the shape of a member a body read with idl_dict_get, with two separate accounts. The member loop
   crosses a concolic member as itself for every type whose rule is not IDL_CONCOLIC_FORKS, so unknown input
   reaching the body is the engine working as designed; the remedy is a fork at the member's own stage, never
   a coercion or a weaker assert. Any other wrong shape means the IdlDictMember list does not declare the
   member that type. Both abort.
   A macro so the refusal names the body's site. `cond` and `v` are DCHECK operands, side-effect-free; `v` is
   read twice. `declared` is the member's IDL as its spec writes it, and `name` its identifier. */
#define IDL_DCHECK_MEMBER(cond, v, name, declared) do {                                                       \
        DCHECKF((cond) || !concolic_is(v),                                                                    \
                "the dictionary member `%s` — declared %s — reached its body as UNKNOWN EXTERNAL INPUT. That " \
                "is Web IDL §3.2.17 Dictionary types' member loop CROSSING it as itself before any type arm "  \
                "is asked, not a conversion that failed: nothing converted this value, deliberately, so "      \
                "opacity would survive the boundary. The body's answer is to refuse the whole call, which "    \
                "drops a world this run could have explored — the member is owed a FORK at its own stage "     \
                "(a step machine stage asking step_fork_run for the arm), never a coercion here and never a "  \
                "weakening of this assert", (name), (declared));                                              \
        DCHECKF((cond),                                                                                       \
                "the dictionary member `%s` reached its body as neither %s nor unknown external input — "      \
                "§3.2.17 (ES-to-IDL list) step 4.1.4.1 converts a member BY ITS DECLARED TYPE, so a third "    \
                "shape means the IdlDictMember list this operation registered does not declare this member "   \
                "that type", (name), (declared));                                                             \
    } while (0)

/* DOM §4.2.3's tree steps, drained where they can yield. The DOM layer records what a mutation changed and
   this machine drains the record before the member returns: synchronous as the spec requires, with no page
   code in between, yielding per node. It is drained here because every declared member converges here; a tree
   mutation from an undeclared member leaves a record nobody consumes, which is asserted. The DOM layer
   registers these so this file need not know what a Node is.
   `step` returns JS_STEP_YIELD (more remains), JS_STEP_FORK (the caller returns it and the walk is re-entered
   at the same node, its own phase preventing repeated effects), or 0 (done, buffer released). It may also make
   a request: DOM §4.2.3 "Mutation algorithms" forbids the insertion steps to run script, while the
   post-connection steps may (HTML §4.12.1.1 "Processing model": script1 removing script2 during
   `body.append(script1, script2)` stops script2), so which phase may request is asserted by the DOM layer.
   `in` is the completion of the walk's last request, owned by the walk, and JS_UNDEFINED otherwise; an answer
   with no request outstanding is asserted. */
typedef struct {
    void *(*take)(JSContext *ctx);                 /* everything recorded so far, or NULL; leaves none behind */
    /* One node; returns a step code. `in`/`out_cb`/`out_argc` are the step-machine request contract. */
    int   (*step)(JSContext *ctx, void *buf, JSStepHdr *h, JSValue in, JSValue **out_cb, int *out_argc);
    void  (*release)(JSContext *ctx, void *buf);
    bool  (*recorded)(void);                       /* is anything waiting to be taken */
    /* The buffer across a fork: the deep fork byte-copies the driving state and re-takes only what a `visit`
       names, so an unvisited buffer would be one allocation two flows walk and free. */
    void  (*visit)(JSContext *ctx, void **buf, JSStepVisit *v);
    /* Gives back what the buffer holds that is not a reference, for a walk abandoned mid-request (HTML
       §8.1.4.6 "Runtime script errors" step 6.1's error reporting mode, which would otherwise stay set on the
       global). `release` runs on the normal 0 edge, this on the abandoned one, called beside
       custom_elements_queue_unlock. It frees no reference; `visit` covers those. */
    void  (*unlock)(JSContext *ctx, void *buf);
} IdlTreeSteps;
void idl_set_tree_steps(const IdlTreeSteps *ops);

/* The document's install is done; no further member declaration can be correct. A component declares in its
   init and installs from the cached id, so a declaration reached later is a per-object mint, which this
   asserts against. Called once by the entry after the components are installed. */
void idl_args_seal(void);

/* Was this member declared before the seal? An install carrying an id minted after the seal is a member
   minted per wrapper or per realm. Asked at the install, where the member's name is. */
bool idl_declared_before_seal(int stepid);

/* Release, in two halves with different lifetimes (see idl_args.c).
   `idl_args_free` gives back what the pool interned (member atoms), so it needs a live runtime, and asserts no
   step machine is live: a flow parked in a member reads this pool at teardown, so the frontier goes first.
   `idl_args_pool_free` gives back the pool's blocks, each holding a JSTrampStepDef that JS_RegisterStepDef
   borrows past the runtime, so it runs after JS_FreeRuntime, beside idl_async_iter_free. */
void idl_args_free(JSContext *ctx);
void idl_args_pool_free(void);

/* An attribute setter's body, run once the assigned value is converted: one value, no argument vector. */
typedef JSValue (*IdlSetter)(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic);

/* Declare an attribute setter: the IDL type of the value and the body to run once converted.
   `null_to_empty` is Web IDL §3.4.6 [LegacyNullToEmptyString], part of the type rather than the body. */
int  idl_setter_id(JSContext *ctx, IdlArgType type, bool null_to_empty, IdlSetter body, int magic);

/* An attribute setter whose algorithm is a step machine, the setter shape of idl_method_id_step (`innerHTML =`
   parses markup, work of the page's size that must yield). `null_to_empty` as for idl_setter_id. */
int  idl_setter_id_step(JSContext *ctx, IdlArgType type, bool null_to_empty, const IdlStepDecl *decl, int magic);

/* Web IDL §3.3.10 [PutForwards]'s setter for a readonly attribute, shared by every attribute carrying it.
 * `attr_id` is §3.7.6 Attributes' `id` and `forward_id` is §3.3.10's identifier argument; both are interned
 * here, because the setter's Get (step 4.5.8.1) and Set (step 4.5.8.4) are keyed requests holding the atom
 * across a suspension.
 *
 * It is a machine: the Get can be an accessor or Proxy trap, and the Set is the forwarded-to attribute's setter
 * (`location = …` forwards to `href`, a navigation that suspends). Step 4.5.8.4's Throw flag is `false`. The
 * value is passed unconverted, because step 4.5.8 returns before step 4.6's conversion; the forwarded-to
 * attribute converts it. */
int  idl_setter_id_put_forwards(JSContext *ctx, const char *attr_id, const char *forward_id);

/* An attribute getter, taking a magic because a reflected attribute is one function over a table of names.
   It runs none of the page's code (it reads the component's own state), so it is a plain C function. */
typedef JSValue (*IdlGetter)(JSContext *ctx, JSValueConst this_val, int magic);

/* Web IDL §3.7.5 Constants' descriptor, named by every constant this engine installs: "Let desc be the
   PropertyDescriptor{[[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false, [[Value]]: value}."
   quickjs spells present attributes, so this is JS_PROP_ENUMERABLE alone. A site names this rather than
   spelling bits, so a constant installed with a bare `0` (non-enumerable) stands out.
   Named residual: this states the descriptor, not the targets. Not covered: §3.7.5 puts a constant on the
   interface object and the interface prototype object, which a component installs by hand, and nothing checks
   both were made. Next diff: a dev-only check at each install site that every name in a constants table is an
   own data property of the target with exactly these attributes. Absence shows as `Node.ELEMENT_NODE` and
   `Node.prototype.ELEMENT_NODE` disagreeing, which no instrument here asks about (engine/idlgen.mjs audits
   which members exist, not their attributes). */
#define IDL_CONSTANT_PROP_FLAGS  JS_PROP_ENUMERABLE

/* The descriptor of the property an interface puts on a global, named by every site that defines one. Web IDL
   §3.8 Platform objects implementing interfaces' define the global property references performs
   "DefineMethodProperty(target, id, interfaceObject, false)", and ECMAScript §10.2.8 DefineMethodProperty (
   homeObj, name, closure, enumerable ) writes { [[Value]]: closure, [[Writable]]: true, [[Enumerable]]:
   enumerable, [[Configurable]]: true } — so writable and configurable, not enumerable (an ordinary [[Set]]
   would make `URL` and every interface name enumerable on the global). The same descriptor serves interface
   objects, [LegacyWindowAlias] aliases, legacy factory functions, legacy callback interface objects and
   namespace objects. Sites name idl_define_global_property_reference, not these bits. */
#define IDL_INTERFACE_OBJECT_PROP_FLAGS  (JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)

/* The install site, captured at the caller: every member-placing entry below is a macro over an `_at`
 * function taking `__FILE__`/`__LINE__`, so the install-side DCHECKs (such as §3.7.6 / §3.7.7 placing a member
 * on a global no [Global] interface declares) name the calling component rather than idl_args.c. A helper
 * function would capture its own address instead, and every `_at` entry requires the pair so a caller without
 * one does not compile.
 *
 * IDL_SITE_INTERNAL is what an idl_args.c-internal path with no caller to name passes instead of IDL_SITE,
 * which would stamp idl_args.c and read as an install site. Every current forwarder carries its caller's pair,
 * so it has no user; NULL is not allowed, because the check DCHECKs the file pointer. */
#define IDL_SITE           __FILE__, __LINE__
#define IDL_SITE_INTERNAL  "core/idl_args.c (an internal path with no caller site to carry)", -1

/* §3.7.6 computes one field from §3.4.10 [LegacyUnforgeable]: "Let configurable be false if attr is
   unforgeable and true otherwise", so the extended attribute is this argument, not a second installer. */
typedef enum {
    IDL_ATTR_REGULAR,       /* §3.7.6: [[Configurable]] true */
    IDL_ATTR_UNFORGEABLE,   /* §3.4.10 [LegacyUnforgeable]: [[Configurable]] false */
} IdlAttrForge;

/* A readonly attribute of the [Global] interface whose value the realm already holds (`window`, `document`,
   `customElements`). §3.7.6 makes every attribute an accessor; a data property would answer
   getOwnPropertyDescriptor wrongly and, from JS_SetPropertyStr, be writable. Not
   idl_install_replaceable_value, which installs [Replaceable]'s setter. Takes ownership of `value`. */
void idl_install_value_attribute_at(JSContext *ctx, JSValueConst target, const char *name, JSValue value,
                                    IdlAttrForge forge, const char *at_file, int at_line);
#define idl_install_value_attribute(ctx, target, name, value, forge) \
    idl_install_value_attribute_at((ctx), (target), (name), (value), (forge), IDL_SITE)

/* Agent teardown for the §3.4.2 [LegacyLenientSetter] declaration table. */
void idl_lenient_setters_free(void);

/* Agent teardown for §3.7.3's [Replaceable] target table. */
void idl_replaceable_targets_free(void);

/* Agent teardown for §3.7.6's receiver-stating attribute table. */
void idl_this_getters_free(void);

/* Web IDL §3.7.1 Interface object for an interface that declares no constructor: a function object whose
   `prototype` is `proto` and whose call and construct both throw a TypeError. A NULL C function pointer is not
   "no constructor"; it is a crash where the spec says TypeError. */
JSValue idl_interface_object(JSContext *ctx, const char *name, JSValueConst proto);

/* §3.8's define the global property references: the one door an interface's name reaches the global through
   (descriptor at IDL_INTERFACE_OBJECT_PROP_FLAGS), and where §3.3.7 step 1 is asked of the identifier. It takes
   an object rather than minting one, so an interface with a constructor keeps it; the component that knows the
   interface mints, and this defines. Takes ownership of `object`; `global` is borrowed. */
void idl_define_global_property_reference(JSContext *ctx, JSValueConst global, const char *id, JSValue object);
/* Web IDL §3.4.11 [LegacyWindowAlias]: §3.8 step 3.1.4 for one of the attribute's identifiers, defining the
 * same interface object under the alias ("Perform DefineMethodProperty(target, id, interfaceObject, false)"),
 * so `webkitURL === URL`. Pass a JS_DupValue of the interface object, never a fresh mint; the reference is
 * consumed on every path.
 *
 * One call per identifier, because step 3.1.4.1 is a loop over them; a NULL-terminated variadic is the scan
 * shape that has already miscompiled into a hang here. It is a separate entry because step 3.1.4 adds the
 * condition "and target implements the Window interface"; it asks that and calls
 * idl_define_global_property_reference, which stays the one place a name reaches a global. */
void idl_define_legacy_window_alias(JSContext *ctx, JSValueConst global, const char *id,
                                    JSValue interface_object);
/* Web IDL §3.11.1 Legacy callback interface object: a built-in function object ("Let F be
   CreateBuiltinFunction(steps, 0, id, « », realm)" over steps that throw a TypeError), so `typeof` answers
   "function". A callback interface has no prototype object, so this call is the only statement of which
   interface the constants installed on it belong to. */
JSValue idl_callback_interface_object(JSContext *ctx, const char *name);

/* Mint a declared step member's function object without installing it, for an internal door a C caller holds.
   Use it instead of JS_NewCFunction2(..., JS_CFUNC_step, stepid), which leaves the member anonymous in every
   diagnostic. Its `length` is §3.7.7 Operations' number derived from the declaration (idl_member_length_of). */
JSValue idl_step_function(JSContext *ctx, const char *name, int stepid);
/* The interface object for a declared constructor. §3.7.1's `length` is derived from the effective overload
   set as for §3.7.7, so the caller states none (`new Event()`'s is 1). */
JSValue idl_step_constructor(JSContext *ctx, const char *name, int stepid);

/* The slowest single member step since the last reset, and which member it was. A step is meant to be short,
   so this tells a scheduler assertion about a long stretch without a suspend point which member was running;
   a small answer says the culprit is not an IDL member. Dev-only; a release build reports 0. */
void idl_slowest_reset(void);
int64_t idl_slowest_step(const char **name);
/* The same window's total across every member step, and how many there were, which separates one slow call
   from many short ones. */
int64_t idl_step_total(long *count);

/* §3.7.3 Interface prototype object's @@toStringTag: the interface's identifier, non-writable,
   non-enumerable, configurable. Every interface calls this.
   It also asserts §3.7.3's [[Prototype]] against the generated browser/idl_inheritance.h (the inherited
   interface's prototype, or this realm's %Object.prototype% / %Error.prototype%), the fact engine/idlgen.mjs's
   gap audit relies on and cannot check.
   It is also where §3.7.3's [Unscopable] block runs: one OrdinaryObjectCreate(null) per interface prototype
   object, holding the identifiers of its exposed [Unscopable] members, defined under %Symbol.unscopables%
   before the members are. The member list is browser/idl_unscopables.h, generated from the .idl. */
void idl_interface_tag(JSContext *ctx, JSValueConst proto, const char *iface);
#if APICLIENT_DEV
/* The other end of that block: asserts every id in each tagged prototype's %Symbol.unscopables% object names
   an own property of that prototype, which cannot be checked at the tag because no member is defined yet.
   Called where the per-realm intrinsic list ends (every member installed), reaching the prototypes through
   core/realm.h's §3.7.3 census. */
void idl_assert_unscopables_name_members(JSContext *ctx);
#endif

/* The same class string on an object that is not an interface prototype object, so §3.7.3's [[Prototype]]
   is not asserted. Only the WindowProxy's prototype needs it: it carries Window's class string while the real
   Window interface prototype object is a different object (core/frame/window.c). Not idl_interface_tag, so the
   C states which kind of object is tagged; engine/idl_installed.mjs reads both forms. */
void idl_class_string(JSContext *ctx, JSValueConst obj, const char *iface);

/* Web IDL §3.13.1 Namespace object's class string, the namespace's identifier (`[object console]`), with the
   same non-writable, non-enumerable, configurable descriptor. Not idl_interface_tag: a namespace object holds
   its operations directly, and engine/idl_installed.mjs reads the interface tag to attribute installs to an
   interface, so a namespace states its own kind. */
void idl_namespace_tag(JSContext *ctx, JSValueConst ns, const char *identifier);

/* Web IDL §3.7.10.2 Asynchronous iterator prototype object's class string: the interface's identifier plus
   " AsyncIterator". Not idl_interface_tag: the object is not an interface prototype object and its `next`
   and `return` are not the interface's members. */
void idl_async_iterator_tag(JSContext *ctx, JSValueConst aproto, const char *iface);

/* §3.2.27's create a frozen array, over an Array the caller filled: SetIntegrityLevel(array, frozen). Every own
   property, `length` included, loses writable and configurable; preventing extensions alone leaves
   `Object.isFrozen` false and the array truncatable. One implementation for the FrozenArray type. Returns <0
   with an exception pending. */
int idl_freeze_array(JSContext *ctx, JSValueConst arr);

/* Web IDL §3.3.7 [Exposed]'s conditional exposure attributes: whether a member exists in a realm. The is
 * exposed in realm algorithm's step 1 (the exposure set) is idl_exposed_in_realm and
 * idl_member_exposed_in_realm below; this enum is the conditional-attribute axis, kept apart because the two
 * are orthogonal (`[Exposed=(Window,Worker), SecureContext]`).
 *
 * Web IDL §3.3.13 [SecureContext] removes the member: no property, never a throwing or undefined getter, since
 * `'deviceMemory' in navigator`, a truthiness test and a try/catch are three different page branches. The
 * component states the attribute as data on the install, and this file asks the condition once for every
 * member, so no site re-derives what it means.
 *
 * [SecureContext] stays component-stated because no generated artifact can decide it, while exposure sets are
 * corpus facts generated by engine/idlgen.mjs. [CrossOriginIsolated] is absent because no member in this build
 * carries it; core/frame/agent_cluster.h answers the capability (false for every environment built today), and
 * a member needing it adds a value here. */
typedef enum {
    IDL_EXPOSED = 0,        /* the member's IDL carries no exposure condition — it is in every realm */
    IDL_SECURE_CONTEXT,     /* [SecureContext] — absent, not throwing, in a non-secure realm */
} IdlExposure;

/* §3.3.7's is exposed in realm conditions, asked. A caller that puts something on a realm may not ask it: an
 * install states its exposure as data and the gate is asked inside this file. A caller that installs nothing
 * may: core/platform.c's witness list, an oracle over the finished realm that must decide the same condition
 * without restating it. See idl_args.c. */
bool idl_exposed(JSContext *ctx, IdlExposure exposure);

/* §3.3.7 [Exposed] step 1 asked of one identifier: "If construct's exposure set is not *, and
 * realm.[[GlobalObject]] does not implement an interface that is in construct's exposure set, then return
 * false". Both sides are generated by engine/idlgen.mjs into browser/idl_exposure.h (exposure set per
 * identifier, §3.3.8 [Global]'s global names per global interface), so the audit and the engine agree.
 *
 * A name with no row is exposed: absence of evidence never removes a property.
 *
 * Asked at idl_define_global_property_reference, where every interface object, legacy factory function and
 * namespace object converges, with nothing stated by callers. It cannot answer for a member, which has no row
 * here; that is idl_member_exposed_in_realm. */
bool idl_exposed_in_realm(JSContext *ctx, const char *identifier);

/* §3.3.7 step 1 asked of a member — §3.7.6's "If attr is not exposed in realm, then continue." and §3.7.7's
 * twin — for a member a [Global] interface puts on the realm's global. A second table, keyed by member, which
 * engine/idlgen.mjs generates by running §3.3.7's get the exposure set over every member of every [Global]
 * interface. The name asked is the member name, without §3.7.6's "get "/"set " function-name prefix.
 *
 * A name with no row is exposed in release. A no-row name is either a member whose exposure set is `*` or a
 * name no [Global] interface writes onto the global at all; the realm's IDL_GLOBALS `own` band tells them
 * apart, which the implementation DCHECKs at the one call that knows the target is the global.
 *
 * Named residual: a sound over-approximation. Not covered: §3.7.6 asks per attribute of one definition, while
 * this is handed only a name, so a global implementing two interfaces that both declare a name keeps it when
 * either is exposed. Next diff: the declaring interface as data at the install (the argument
 * idl_check_global_target's DFAIL already names), keying the question by (interface, member). Absence shows as
 * a property present whose getter belongs to an interface the realm does not expose, never a missing name. */
bool idl_member_exposed_in_realm(JSContext *ctx, const char *member);

/* §3.3.8 [Global]'s global names of one global interface, the realm side of §3.3.7 step 1's intersection.
 * Resolved once per realm by core/realm.c; an interface the corpus does not declare [Global] aborts here,
 * since a realm with no global names would silently lose every non-`*` construct. */
unsigned idl_global_names_of(const char *global_interface);

/* HTML §8.1.3.5 "Secure contexts" step 1.2's "If global is a WorkerGlobalScope" and step 1.3's "If global is a
 * WorkletGlobalScope", read off the global names mask. Sound because the mask and the interface agree over the
 * corpus's rows, which idl_args.c derives. They take the mask, not a realm, because the question is about
 * §3.3.8's vocabulary; core/frame/secure_context.c runs the algorithm. */
bool idl_global_names_are_worker(unsigned global_names);
bool idl_global_names_are_worklet(unsigned global_names);

/* §3.8 step 3.1.4's second conjunct, "and target implements the Window interface", read off the same mask.
 * Exactly one IDL_GLOBALS row, `Window`, carries IDL_GLOBAL_WINDOW, so the bit and implementing Window are the
 * same realms; idl_args.c DCHECKs that, so a second such row would be reported rather than admitting an alias
 * where §3.8 excludes it. */
bool idl_global_names_are_window(unsigned global_names);

/* §3.7.6 Attributes' accessor function name: "get " or "set " prepended to the identifier. The installers do
 * this themselves; it is declared for the few members defined at a raw JS_DefinePropertyGetSet, so the prefix
 * is written in one place. `buf` is the caller's, at least IDL_ACCESSOR_NAME_MAX bytes, and the result is for
 * the function mint alone, never a property key, pool entry or a getter's own name (see idl_args.c). A plain
 * C setter reaches every installer through idl_setter_id's step id, so no plain-C-setter installer is needed.
 *
 * Named residual: the raw sites. Not covered: HTMLTemplateElement's `content`, AbortSignal's `aborted` and
 * AbortController's `signal` are defined by JS_DefinePropertyGetSet, so their descriptor and name come from
 * the call site and nothing checks them against the installers (`aborted` and `signal` still spell "get …" by
 * hand). Next diff: install `content` and `signal` through idl_install_accessor (their getters already have
 * the IdlGetter shape; a -1 setter id is readonly), give `aborted` that shape and do the same, then delete
 * this entry. Absence shows as a raw-site member whose descriptor or getter name differs from every installed
 * member. Re-derive the sites with `grep -rnE 'JS_DefinePropertyGetSet[[:space:]]*\(' engine/host
 * --include=*.c`, minus idl_args.c. */
#define IDL_ACCESSOR_NAME_MAX 96
typedef enum { IDL_ACCESSOR_GET, IDL_ACCESSOR_SET } IdlAccessorKind;
const char *idl_accessor_name(char *buf, size_t cap, const char *id, IdlAccessorKind kind);

/* AN ATTRIBUTE THAT STATES ITS IDL'S EXPOSURE. This is the general form; the plain `idl_install_accessor`
   below is the same install for a member whose IDL carries no exposure condition, which is most of them. */
void idl_install_accessor_exposed_at(JSContext *ctx, JSValueConst target, const char *name,
                                     IdlGetter getter, int getter_magic, int setter_stepid,
                                     IdlExposure exposure, const char *at_file, int at_line);
#define idl_install_accessor_exposed(ctx, target, name, getter, magic, setter, exposure) \
    idl_install_accessor_exposed_at((ctx), (target), (name), (getter), (magic), (setter), (exposure), IDL_SITE)

void idl_install_accessor_at(JSContext *ctx, JSValueConst target, const char *name,
                             IdlGetter getter, int getter_magic, int setter_stepid,
                             const char *at_file, int at_line);
#define idl_install_accessor(ctx, target, name, getter, magic, setter) \
    idl_install_accessor_at((ctx), (target), (name), (getter), (magic), (setter), IDL_SITE)

/* AN INTERFACE OBJECT THAT STATES ITS IDL'S EXPOSURE — the same IdlExposure and the same one gate the two
 * member installers ask, applied to the OTHER half of what Web IDL §3.3.13 [SecureContext] removes.
 *
 * §3.3.13's own example says both halves in one breath, of an interface-level annotation: "HeartbeatSensor will
 * not be exposed in a non-secure context, nor will its members. In such a context, there will be no
 * \"HeartbeatSensor\" property on Window." The members half is what `idl_install_accessor_exposed` and
 * `idl_install_method_exposed` already perform; this is the property on the global, and §3.7 Interfaces is what
 * makes it the same question — "For every interface that is exposed in a given realm … a corresponding property
 * exists on the realm's global object", so an interface that is NOT exposed has no such property to have.
 *
 * NOTHING IS MINTED WHEN IT IS NOT EXPOSED, exactly as the attribute form states: the interface object is not
 * built, the property is not defined, and `"X" in globalThis` is false. Absent, undefined and throwing are three
 * different branches of a page's feature detection and only the first is what §3.3.13 asks for.
 *
 * `proto` is BORROWED — this installs a reference to an object the caller still owns and still frees, which is
 * how every call site already holds a prototype it goes on to use. The interface object it builds is handed to
 * `target`, which owns it from then on. */
void idl_install_interface_object_exposed(JSContext *ctx, JSValueConst target, const char *name,
                                          JSValueConst proto, IdlExposure exposure);

/* THE SAME DOOR FOR AN INTERFACE THAT DECLARES A CONSTRUCTOR **AND** CARRIES A §3.3.7 CONDITIONAL ATTRIBUTE —
 * the one combination neither of the two entries above can serve, and the reason is a property of each rather
 * than an oversight in either. `idl_install_interface_object_exposed` asks the condition and MINTS over
 * `idl_illegal_ctor`, so routing a constructor-declaring interface through it would answer `new X()` with a
 * TypeError; `idl_define_global_property_reference` takes the object the component minted and asks step 1
 * alone, because a conditional attribute is a fact only the component knows it carries. So an interface that
 * is both — `[Exposed=Window, SecureContext] interface CookieChangeEvent : Event { constructor(…); }` is the
 * first in this engine — had no door at all.
 *
 * AND THE SHAPE A CALLER REACHES FOR INSTEAD ABORTS THE DEV BUILD, which is why this is a door and not advice.
 * `if (idl_exposed(ctx, IDL_SECURE_CONTEXT)) { … define … }` is the per-site conditional idl_exposed's own
 * contract forbids, and it is not merely style here: the ask is recorded INSIDE the define, so a realm that
 * took the false arm would have built this realm's `X.prototype` and recorded no §3.8 ask for `X` — which is
 * exactly the disagreement `realm_assert_interface_objects_asked` fires on, naming the interface. The gate has
 * to sit where the ask can be recorded before it.
 *
 * TAKES OWNERSHIP of `object` on EVERY path, the refusal included, exactly as the entry above does — a caller
 * that could get it back would have two shapes to write instead of one. `global` is BORROWED. */
void idl_define_global_property_reference_exposed(JSContext *ctx, JSValueConst global, const char *id,
                                                 JSValue object, IdlExposure exposure);

/* THE SAME ATTRIBUTE, WITH ITS GETTER DECLARING THAT ITS BODY RUNS NONE OF THE PAGE'S CODE.
 *
 * WHY THERE IS ANYTHING TO DECLARE. A property read that lands on an accessor may not invoke it from C:
 * §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ) step 7 is `Return ? Call(getter, receiver)`, and a C
 * activation has no flow base under it, so a loop in the page's getter would drive to completion instead of
 * parking. The engine therefore routes every accessor read through the trampoline and a C reader that reaches
 * one ABORTS naming the site. All of that is about a body that RUNS THE PAGE, and it is vacuous for a C getter
 * that runs none: nothing to suspend, no continuation to hold, and the routed path reaching the same C body
 * through the same dispatch. Such a getter was aborting on a non-problem, which costs real aborts and teaches
 * the next reader to distrust the crashes that mean something.
 *
 * THE DECLARATION IS A CLAIM, AND A FALSE ONE FIRES. Say nothing and the member is undeclared, which means "a C
 * reader of this must be routed, and it crashes" — that stays the default for every attribute in the engine.
 * Say this and the engine holds you to it: while the getter runs, entry into ANY bytecode body aborts naming
 * this member, because bytecode is the one door the page's code comes through. So the day a helper three calls
 * down gains a [[Get]] on an object whose prototype a page can extend, a coercion of a value a page can make an
 * object, or a callback, the claim CRASHES instead of silently keeping an exemption it stopped deserving.
 *
 * IT IS NOT THE ANSWER TO A LONG GETTER. The claim is about what the body REACHES, never about how much work it
 * does — a walk over every child of a large tree reaches no page code and still holds the scheduler for the
 * length of the walk, and the answer to that one is `idl_install_accessor_step` below, whose getter is a machine
 * that yields. The two are different questions and a member can need both answers at different times. */
void idl_install_accessor_no_user_code_at(JSContext *ctx, JSValueConst target, const char *name,
                                          IdlGetter getter, int getter_magic, int setter_stepid,
                                          const char *at_file, int at_line);
#define idl_install_accessor_no_user_code(ctx, target, name, getter, magic, setter) \
    idl_install_accessor_no_user_code_at((ctx), (target), (name), (getter), (magic), (setter), IDL_SITE)

/* WEB IDL §3.4.10's [LegacyUnforgeable] ATTRIBUTE — the OTHER of the two places §3.7.6 puts an attribute, and
 * a different member of the platform rather than a different way of writing the same one.
 *
 * §3.4.10: "the property will be non-configurable and will exist as an own property on the object itself rather
 * than on its prototype", and §3.7.6 states the descriptor exactly — the same getter and setter, [[Enumerable]]
 * true, and [[Configurable]] FALSE where the ordinary form's is true. §3.7.6's "define the regular attributes"
 * REMOVES the unforgeable ones from what goes on the interface prototype object, so an interface whose members
 * are all unforgeable has a prototype carrying only §3.7.3's `constructor` and @@toStringTag — which is exactly
 * what `Object.getOwnPropertyNames(Location.prototype)` reports in a browser.
 *
 * SO THE CALLER PASSES THE INSTANCE, not the prototype, and the two facts arrive together: a member defined
 * configurable and locked down afterwards is a member that was forgeable for the length of one install, and a
 * member on a prototype is one a page can shadow with an own property of its own. HTML §7.2.4 marks every
 * member of Location unforgeable for that reason and says so — "required by legacy code that consulted the
 * Location interface, or stringified it, to determine the document URL, and then used it in a
 * security-sensitive way" — so `foo[location] = bar` and `location + ""` cannot be misdirected. */
/* WEB IDL §3.7 Interfaces' implementation-check an object, step 3 — "If object does not implement interface,
 * then throw a TypeError." — as a NAMED TYPE. It is the same predicate `idl_this_iface` takes one screen up,
 * spelled once so the two entries that take it agree by construction rather than by two parameter lists being
 * kept in step.
 * IT IS A TYPEDEF AND NOT A RAW `bool (*)(JSValueConst)` FOR A SECOND REASON, WHICH IS THAT THE AUDIT COULD
 * NOT READ THE RAW FORM. engine/idl_installed.mjs finds a function by matching its parameter list, and a
 * parameter that is itself a parenthesised function type defeats that match — so a definition spelling the
 * predicate raw is invisible to the reader, its own forwarding install is reported UNRESOLVED, and every
 * member it places goes uncounted. Measured on the first form written here: one unresolved construct in
 * core/idl_args.c, and both of its functions missing from the reader's index. `idl_this_iface` above spells it
 * raw and is in exactly that state today — a NAMED OBSERVATION rather than a claim it does not matter: it
 * DECLARES rather than installs, so no member is lost through it, and converting it is a separate diff. */
typedef bool (*IdlThisIs)(JSValueConst v);

/* WEB IDL §3.4.2 [LegacyLenientSetter] — the same readonly attribute as the plain form above, PLUS §3.7.6
 * Attributes' no-op setter. §3.4.2: "If the [LegacyLenientSetter] extended attribute appears on a read only
 * regular attribute, it indicates that a no-op setter will be generated for the attribute's accessor property.
 * This results in erroneous assignments to the property in strict mode to be ignored rather than causing an
 * exception to be thrown."
 *
 * THE OBSERVABLE IS STRICT MODE AND NOTHING ELSE. A readonly accessor with no setter and one with a no-op
 * setter answer an ordinary `d.fullscreen = 1` identically — silently — because sloppy mode ignores a write to
 * an accessor with no setter. `"use strict"` is where they part: no setter is a TypeError, a no-op setter is
 * ignored. §3.4.2 says in its own words why that matters enough to be in the standard at all: "Pages have been
 * observed where authors have attempted to polyfill an IDL attribute by assigning to the property … Without
 * [LegacyLenientSetter], this could prevent a browser from shipping the feature."
 *
 * `this_is` IS THE INTERFACE THE RECEIVER MUST IMPLEMENT — the component's own `…_is` predicate, exactly as
 * idl_this_iface takes, because §3.7.6's setter throws a TypeError for a foreign receiver BEFORE it reaches
 * §3.4.2's arm. It is per INSTALL rather than per member: the corpus's population includes a
 * DocumentOrShadowRoot mixin member, which lands on Document and on ShadowRoot and brands differently on each.
 *
 * NOTHING HERE STATES WHICH MEMBERS CARRY THE EXTENDED ATTRIBUTE, and that is the difference between one
 * mechanism for the platform and N copies that drift: engine/idlgen.mjs reads it off the real .idl through
 * webidl2 and checks BOTH directions — a member the corpus annotates that some component installs through the
 * plain form, and a member installed through this one that the corpus does not annotate. §3.4.2 makes the
 * second an error rather than a preference ("It must not be used on anything other than a read only regular
 * attribute", and never together with [PutForwards] or [Replaceable]).
 *
 * IT IS NOT `js_noop` AND THE BAN DOES NOT REACH IT — see core/idl_args.c for the two sentences of the
 * standard that make this §NO STUBS' documented exception rather than an instance of what it forbids. */
void idl_install_accessor_lenient_setter_at(JSContext *ctx, JSValueConst target, const char *name,
                                            IdlGetter getter, int getter_magic,
                                            IdlThisIs this_is, const char *iface,
                                            const char *at_file, int at_line);
#define idl_install_accessor_lenient_setter(ctx, target, name, getter, magic, this_is, iface) \
    idl_install_accessor_lenient_setter_at((ctx), (target), (name), (getter), (magic), (this_is), (iface), \
                                           IDL_SITE)

/* WEB IDL §3.7.6 "Attributes"' RECEIVER TEST, FOR AN ATTRIBUTE WHOSE GETTER IS A PLAIN C FUNCTION — the same
 * install as the plain form above, PLUS the interface the receiver must implement.
 *
 * WHY THE PLAIN FORM CANNOT ASK IT. §3.7.6's create an attribute getter refuses a foreign receiver before the
 * member's own steps run — Web IDL §3.7.6 "Attributes": "If jsValue does not implement target, then:", and its
 * second arm, Web IDL §3.7.6 "Attributes": "Otherwise, throw a TypeError." A member with a POOL ENTRY has that
 * performed for it at one place, idl_implementation_check, off the interface idl_this_iface states at its
 * DECLARATION. A plain getter has no pool entry, so it converges on nothing that could ask, and the receiver
 * reaches the body as the page wrote it. This is the declaration for that member.
 *
 * IT IS THE DECLARATION SIDE AND THAT IS THE WHOLE POINT. A body that tests its own receiver answers LATE —
 * after Web IDL §3.6 "Overload resolution algorithm" has converted arguments, so a page's own `valueOf` has
 * already run where a browser throws with none of it having run. An attribute takes no arguments, so that
 * ordering is not what a getter is wrong about; what a getter is wrong about is that a body-side test is
 * WRITTEN N TIMES, and the two ways it gets written wrong are both live. ANSWERING is one: a body that reads
 * its receiver's record and returns undefined, null or 0 when there is none reports a member's value where the
 * standard reports a TypeError, and a page branches on the difference. ASSERTING is the other and is worse: a
 * receiver is PAGE-SUPPLIED INPUT, so a DCHECK on it is an abort switch a page holds, and in release, where the
 * DCHECK is compiled out, the body dereferences the NULL the assert was standing on — which is what
 * `DocumentType.prototype.name` did, through `doc_type->node.owner_document->attrs`.
 *
 * `this_is` IS THE COMPONENT'S OWN PREDICATE, NAMED AND NOT RESTATED, and it is the SAME function
 * idl_this_iface takes for the same sentence of §3.7 — so an interface that owns operations and attributes
 * brands both out of one answer, and converting its getters cannot make them disagree with its operations. It
 * cannot be a class comparison for the reason idl_this_iface states: a member declared on Document is reached
 * on an XMLDocument, and a mixin member is reached on every interface that includes the mixin. It is therefore
 * per INSTALL and not per member.
 *
 * `iface` IS THE IDENTIFIER STEP 1.1.2.3.2's TypeError NAMES, a static the caller owns, exactly as
 * idl_install_accessor_lenient_setter_at's is.
 *
 * WHAT IT DOES NOT ADD IS §3.5's SECURITY CHECK, which is the state every plain-C getter on an interface
 * prototype is already in — core/idl_args.c's idl_implementation_check block names that residual and this
 * NARROWS it rather than retiring it. */
void idl_install_accessor_this_at(JSContext *ctx, JSValueConst target, const char *name,
                                  IdlGetter getter, int getter_magic, int setter_stepid,
                                  IdlThisIs this_is, const char *iface,
                                  const char *at_file, int at_line);
#define idl_install_accessor_this(ctx, target, name, getter, magic, setter, this_is, iface) \
    idl_install_accessor_this_at((ctx), (target), (name), (getter), (magic), (setter), (this_is), (iface), \
                                 IDL_SITE)

void idl_install_accessor_unforgeable_at(JSContext *ctx, JSValueConst target, const char *name,
                                         IdlGetter getter, int getter_magic, int setter_stepid,
                                         const char *at_file, int at_line);
#define idl_install_accessor_unforgeable(ctx, target, name, getter, magic, setter) \
    idl_install_accessor_unforgeable_at((ctx), (target), (name), (getter), (magic), (setter), IDL_SITE)

/* WEB IDL §3.7.3 "Interface prototype object"'s CONDITIONAL, ASKED AT AN INSTALL — the object a member goes
 * on in THIS realm, given the realm's global. OWNED: the caller frees.
 *
 * A COMPONENT THAT OWNS A MIXIN MEMBER MUST ASK IT AND MUST NOT ANSWER IT. §2.3 "Interface mixins" makes a
 * mixin's members the INCLUDING interface's own — "all objects implementing an interface I ... must
 * additionally include the members of interface mixin M" — so HTML §8.2's WindowOrWorkerGlobalScope member
 * is a `Window` member in a Window realm and a `WorkerGlobalScope` member in a worker one, and §3.7.3's
 * [Global] conditional then sends it to two different objects. This call is that conditional; asking the
 * REALM KIND instead is a hand-picked list of the realms an engine happens to build, and it installs onto the
 * global in every other one. idl_args.c states the derivation and the residual over the implementation.
 *
 * IT IS NOT §3.3.7 STEP 1 AND DOES NOT REPLACE IT. A member this realm does not expose is answered with the
 * GLOBAL here and refused by the install entry, which is where that step is asked and asserted. So a caller
 * installs onto whatever this returns and the entry decides whether the member is placed at all. */
JSValue idl_global_member_target_at(JSContext *ctx, JSValueConst global, const char *name,
                                    const char *at_file, int at_line);
#define idl_global_member_target(ctx, global, name) \
    idl_global_member_target_at((ctx), (global), (name), IDL_SITE)

/* §3.7.3's not-[Global] arm's OBJECT, as data the component that builds it states — AGENT-SCOPED, registered
 * at that component's declaration and cleared at its release. `proto_of_realm` is REALM-AWARE and answers
 * JS_UNDEFINED (OWNED, the caller frees) in a realm that has no such object; `iface` is the interface whose
 * §3.7.3 prototype it returns, and it is load-bearing rather than decorative — it is what the
 * second-claimant abort names. Passing (NULL, NULL) clears. */
void idl_set_global_ancestor_terms(JSValue (*proto_of_realm)(JSContext *ctx), IdlThisIs this_is,
                                   const char *iface);

/* A [Replaceable] ATTRIBUTE WHOSE DECLARING INTERFACE IS THE REALM'S — the two entries above this one, asked
 * as ONE call because the object and the brand are one fact. §3.7.3's conditional decides the object; on the
 * not-[Global] arm §3.7.6's create an attribute setter step 1.1.2.3 then asks whether the receiver implements
 * `target`, and `target` there is the DECLARING interface, so a caller that resolved the object would still
 * have to state a brand it does not own. Both come from the registration, so a component owning a mixin
 * member writes one line and names no realm kind and no interface but its own member's. */
void idl_install_replaceable_member_at(JSContext *ctx, JSValueConst global, const char *name,
                                       IdlGetter getter, int getter_magic,
                                       const char *at_file, int at_line);
#define idl_install_replaceable_member(ctx, global, name, getter, magic) \
    idl_install_replaceable_member_at((ctx), (global), (name), (getter), (magic), IDL_SITE)

/* WEB IDL §3.7.6's [Replaceable] ATTRIBUTE. It is READONLY, and yet assigning to it works: the setter DEFINES
   an ordinary data property on the receiver, which replaces the accessor outright. So `window.length` is an
   accessor until a page writes to it and a `{writable:true}` data property afterwards, and the corpus reads
   the descriptor on both sides of that line. It cannot be modelled as a plain writable data property — that is
   an accessor the whole time it should be one and never has a getter — nor as a readonly accessor, which
   silently drops the write. Every replaceable member shares one setter; the property NAME rides on the
   function as its data, so there is one implementation and no per-member setter to forget.
   `idl_install_replaceable_value` is the form for an attribute whose value is FIXED for the realm (§7.2.2.5's
   BarProps, `frames`, `origin`): the getter answers the value it was given. `value` is CONSUMED.

   THE RECEIVER IS §3.7.6's, RESOLVED BY THE MINT AND NOT BY THE MEMBER — "the this value, if it is not null or
   undefined, or realm's global object otherwise", then a TypeError when it does not implement the interface.
   So `desc.set.call(null, v)` replaces the member on the GLOBAL rather than defining a property on `null`, and
   `Object.create(globalThis).origin = x` throws instead of quietly shadowing the accessor on an unrelated
   object.
   WHICH INTERFACE IS BRANDED AGAINST IS THE ARM'S, AND THIS ONCE SAID IT WAS ALWAYS Window. It read `the
   interface branded against is Window, and that is ASSERTED at the install rather than assumed at the read:
   target must be the realm's global object`, which is exactly right about THIS entry and false of the pair:
   the sentence is rewritten rather than dropped because a reader who re-derives it from this form alone will
   re-introduce it. This form is §3.8 "Platform objects implementing interfaces"' [Global] arm, where §3.7.6's
   `target` is the instance's own [[PrimaryInterface]] — the REALM's to state — so the install asserts only
   that the target IS the realm's global and the READ asks which [Global] interface that realm has.
   `idl_install_replaceable_on` below is §3.7.3's not-[Global] arm, where the same member lands on the
   interface prototype object of the interface that DECLARES it and `target` is the INSTALL's to state.
   THE `IdlGetter` FORM'S GETTER STEPS STILL SEE AN UNRESOLVED RECEIVER, because §3.7.6's check belongs to the
   mint and a raw C getter cannot be wrapped without carrying a function pointer through the closure's data —
   which JSCFunctionType must never hold. Those getters answer per-realm and so answer correctly; what they do
   not do is THROW for a receiver that implements nothing. See the note on the resolution in idl_args.c. */
void idl_install_replaceable_at(JSContext *ctx, JSValueConst target, const char *name,
                                IdlGetter getter, int getter_magic, const char *at_file, int at_line);
#define idl_install_replaceable(ctx, target, name, getter, magic) \
    idl_install_replaceable_at((ctx), (target), (name), (getter), (magic), IDL_SITE)
/* THE SAME [Replaceable] ATTRIBUTE ON A §3.7.3 INTERFACE PROTOTYPE OBJECT — Web IDL §3.7.3 "Interface
   prototype object": "If interface is not declared with the [Global] extended attribute, then: Define the
   regular attributes of interface on interfaceProtoObj, given realm".
   WHEN A COMPONENT TAKES THIS ARM RATHER THAN THE ONE ABOVE IS NOT A PREFERENCE AND NOT A REALM TEST: it is
   WHICH INTERFACE DECLARES THE MEMBER IN THIS REALM. A mixin included by a [Global] interface and by a
   non-[Global] one lands on two different objects for that reason and for no other — HTML §8.2 "The
   WindowOrWorkerGlobalScope mixin" is included by `Window`, which IS [Global], and by `WorkerGlobalScope`,
   which is NOT, so HR-TIME's `performance` is an own property of a Window and an own property of
   `WorkerGlobalScope.prototype` in a worker. browser/idl_exposure.h's IDL_GLOBALS band is what states the
   split, and BOTH arms assert it: this one that the realm's [Global] interface does NOT declare the member,
   the one above (through idl_global_member_refused) that it DOES.
   `this_is`/`iface` ARE §3.7.6's `target`, the declaring interface's own predicate and identifier, exactly as
   idl_install_accessor_lenient_setter_at takes them and for the same sentence of §3.7.6. They are the
   INSTALL's to state here because a realm holds several §3.7.3 prototypes and only the object being installed
   onto says which one this is.
   THE GETTER STEPS STILL SEE AN UNRESOLVED RECEIVER, exactly as on the form above and for the same reason —
   a plain-C getter on anything but the realm's global is minted raw. What this arm adds is the SETTER's
   preamble, which is where a [Replaceable] member's receiver was being checked against Window in a realm that
   has none. */
void idl_install_replaceable_on_at(JSContext *ctx, JSValueConst target, const char *name,
                                   IdlGetter getter, int getter_magic, IdlThisIs this_is, const char *iface,
                                   const char *at_file, int at_line);
#define idl_install_replaceable_on(ctx, target, name, getter, magic, this_is, iface) \
    idl_install_replaceable_on_at((ctx), (target), (name), (getter), (magic), (this_is), (iface), IDL_SITE)
/* The half of that setter a member with its OWN setter steps still needs: Web IDL's
   CreateDataPropertyOrThrow(receiver, name, V), which REPLACES the accessor on that object. HTML §7.2.5's
   `opener` setter ends in exactly this operation for a non-null value, so it is one implementation reached from
   two declarations rather than two that can drift. Returns <0 with an exception pending, like every define. */
int  idl_replace_with_value(JSContext *ctx, JSValueConst obj, const char *name, JSValueConst v);
void idl_install_replaceable_value_at(JSContext *ctx, JSValueConst target, const char *name, JSValue value,
                                      const char *at_file, int at_line);
#define idl_install_replaceable_value(ctx, target, name, value) \
    idl_install_replaceable_value_at((ctx), (target), (name), (value), IDL_SITE)

/* AN ACCESSOR WHOSE GETTER IS A MACHINE. A getter takes no arguments, so it has nothing to CONVERT — and that
   is why it was a plain C function, which was the wrong conclusion: it may still have work of the PAGE'S size
   to do. `innerHTML` serialises the whole document, `outerHTML` the same, `childNodes.length` counts every
   child; none of it can reach the page's code and all of it held the scheduler for as long as the tree was big.
   A getter declared this way is a function object of exactly the kind the SETTER already is, called the same
   way by the property machinery, so it can yield at every step of its walk.
   The plain-C form above is what remains to be converted, not a second way of doing this. */
int  idl_getter_id_step(JSContext *ctx, const IdlStepDecl *decl, int magic);
void idl_install_accessor_step_at(JSContext *ctx, JSValueConst target, const char *name,
                                  int getter_stepid, int setter_stepid, const char *at_file, int at_line);
#define idl_install_accessor_step(ctx, target, name, getter_stepid, setter_stepid) \
    idl_install_accessor_step_at((ctx), (target), (name), (getter_stepid), (setter_stepid), IDL_SITE)

/* Install a declared member on `target`. The coercion is a request, so a page's `toString` — loop, await and
   all — suspends and resumes at the exact argument it was on.
   THERE IS NO `length` TO PASS, AND THAT IS THE POINT. Web IDL §3.7.7 Operations' length is "the length of the
   shortest argument list in the entries in S" over the effective overload set at argument count 0, which is a
   function of the DECLARATION alone — min(first optional, declared positions) — so this pool computes it and
   an install has nothing to remember. It used to be a parameter, and the seven installs of DOM §4.2.6 Mixin
   ParentNode's and §4.2.8 Mixin ChildNode's members, all reaching ONE declaration, DISAGREED WITH EACH OTHER
   about it: five said 1 and two said 0 for the identical arity, so `Element.prototype.append.length` was a
   number no reading of the IDL produces. `new Event()`'s interface object had the same defect one section over,
   carrying the declared arity 2 where §3.7.1 Interface object computes 1. See idl_member_length_of. */
void idl_install_method_at(JSContext *ctx, JSValueConst target, const char *name, int stepid,
                           const char *at_file, int at_line);
#define idl_install_method(ctx, target, name, stepid) \
    idl_install_method_at((ctx), (target), (name), (stepid), IDL_SITE)
/* §3.4.10's [LegacyUnforgeable] FOR AN OPERATION — the twin of idl_install_accessor_unforgeable, and it goes
   through §3.7.7's own descriptor: on the INSTANCE the caller passes rather than on a prototype, and
   {[[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false}. Two installers because §3.7.7 states
   two, not because a caller may pick. */
void idl_install_method_unforgeable_at(JSContext *ctx, JSValueConst target, const char *name, int stepid,
                                       const char *at_file, int at_line);
#define idl_install_method_unforgeable(ctx, target, name, stepid) \
    idl_install_method_unforgeable_at((ctx), (target), (name), (stepid), IDL_SITE)
/* THE SAME INSTALL FOR A METHOD THAT STATES ITS IDL'S EXPOSURE — §3.3.13's [SecureContext] REMOVES the member,
   for an operation exactly as for an attribute, and this is that one rule asked in the one place. */
void idl_install_method_exposed_at(JSContext *ctx, JSValueConst target, const char *name, int stepid,
                                   IdlExposure exposure, const char *at_file, int at_line);
#define idl_install_method_exposed(ctx, target, name, stepid, exposure) \
    idl_install_method_exposed_at((ctx), (target), (name), (stepid), (exposure), IDL_SITE)
/* The installer for a method whose algorithm is a step machine of its OWN (its own JSTrampStepDef) rather than
   a member of the args machine — `click` and `dispatchEvent` today. Separate from idl_install_method because
   they are separate things, and each asserts it was handed its own kind. */
void idl_install_step_method_at(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid,
                                const char *at_file, int at_line);
#define idl_install_step_method(ctx, target, name, length, stepid) \
    idl_install_step_method_at((ctx), (target), (name), (length), (stepid), IDL_SITE)

/* THE MEMBERS THIS USER AGENT MUST NOT HAVE — the other half of an install, and the one the IDL cannot state.
   A member whose existence the spec makes CONDITIONAL in prose (HTML §8.10.1.1's `taintEnabled()` and `oscpu`,
   which a user agent supports only "if the navigator compatibility mode is Gecko") reaches the published .idl
   with the condition stripped, so an absent one is indistinguishable from an unbuilt one — by the gap auditor
   and by the next person reading the prototype. Declaring it here says which it is, ASSERTS per realm that the
   prototype indeed lacks each name, and hands the auditor the same list to check against the live corpus.
   `why` is the spec sentence, not a label. See idl_args.c. */
void idl_members_excluded(JSContext *ctx, JSValueConst proto, const char *iface,
                          const char *const *names, int n, const char *why);

/* THE NAMES A ROW-FILTERED INSTALL LOOP COVERS — the second thing the gap auditor cannot read off a loop, and
   the mirror of the exclusion above: that one says a name is NOT there, this one says a whole column IS.
   A loop that installs `TBL[i].member` and drops rows with a `continue` installs an unknown SUBSET as far as
   any static reader is concerned, so the auditor REFUSES the site rather than crediting the column — crediting
   a name the loop drops is a false COMPLETE, the one error it cannot catch from the other side, because the
   member it hides never prints as anything. Teaching it to recognise the filter's TEXT is not the repair: a
   `continue` is C rather than a declaration, and a reader that matches one shape of it silently mis-reads the
   next — a parameter-free filter (`if (!TBL[i].enabled) continue;`) drops names while looking like a dedup.
   So the engine states the RESULT instead of the filter: once this returns, `target` carries every name the
   column holds. That is a claim about an OBJECT, not about source text, which is what makes it checkable where
   claims about objects are checked — here, per realm, as an OWN-property lookup of each name, the very lookup
   the page would do. A filter that removed a name, a column the loop never walked, a target it wrote somewhere
   else: each fires at the origin, naming the member the audit would otherwise have credited.
   The auditor reads the SAME call and credits the column at the install site it MATCHES — matched by the
   install's own arguments (same column, same target, same function), so a declaration no install in its
   function answers to is an error rather than a line nobody revisits, and an install that needs one and lacks
   one stays refused. Those two sides are the whole difference between this and handing the auditor a number.
   `why` is the sentence that says why the loop's filter cannot remove a NAME. See idl_args.c. */
void idl_install_covers_column(JSContext *ctx, JSValueConst target, const char *const *column,
                               int n, size_t stride, const char *why);
/* The column's address, its length and its stride from ONE spelling of the table and ONE of the field, so the
   three cannot drift apart — and the auditor reads the same two identifiers the compiler does. */
#define IDL_NAME_COLUMN(tbl, field) \
    ((const char *const *)&(tbl)[0].field), (int)(sizeof(tbl) / sizeof((tbl)[0])), sizeof((tbl)[0])

/* THE SAME COLUMN'S CONSTRUCTOR AXIS — the counterpart of the coverage declaration above, and the OTHER thing a
   row-filtered loop hides. The two are different questions and the one above answers only the first:
   Web IDL §3.7 Interfaces puts a property on the realm's global object for every exposed interface, and
   Web IDL §3.7.1 Interface object gives that object construct steps only where the interface declares a
   constructor operation — "Interface objects whose interfaces are not declared with a constructor operation
   will throw when called, both as a function and as a constructor". So an interface object that CONSTRUCTS and
   one carrying that throw are the SAME answer to idl_install_covers_column and OPPOSITE answers to `new`, and a
   loop minting its column's interface objects through a shared helper leaves the second question unsaid. A
   reader of the source cannot recover it for the same reason it cannot recover the first: the `continue`
   deciding which cells reach the mint is C rather than a declaration.
   SO THE ENGINE STATES THIS RESULT TOO, AND IT IS A PARTITION RATHER THAN A CLAIM ABOUT ALL OF THEM: every name
   the column holds reached §3.7.1's declared constructor mint, EXCEPT the ones `refuses` names, which carry the
   throw instead. A uniform column passes NULL and 0, which is the ordinary case and not a weaker declaration.
   `JS_IsConstructor` IS NOT THE DISCRIMINATOR AND MUST NOT BE REACHED FOR: both populations are minted with a
   constructing cproto, so it answers true for the interface object that runs an algorithm and for the one whose
   whole body is §3.7.1's TypeError. What separates them is WHICH MINT the identifier went through, which is a
   fact about this engine and not a property anything can ask the finished object — so that is what is recorded
   and that is what is checked. THE RECEIVER OF `new` IS NEVER ASSERTED ON, here or anywhere near here: a page
   supplies it, §3.7.1 answers it with a TypeError, and an abort would hand a page a switch on this engine.
   THE TWO SIDES ARE COMPUTED BY DIFFERENT CODE, which is what makes it a check rather than a restatement. This
   declaration is a static claim about a column; the other side is the set of identifiers idl_step_constructor
   was actually handed. A filter that STOPS minting a name therefore fires here naming that name — the direction
   nothing else can see, because a mint's own precondition guard catches a name that ARRIVES and is silent about
   one that stops arriving, and because that is exactly the direction in which a credited column becomes a false
   COMPLETE: the audit reports an interface constructible and the page gets a TypeError.
   AND `refuses` IS CHECKED BACK AGAINST THE COLUMN, so a refusal naming a name the column no longer holds is an
   error rather than a line nobody revisits — the two-sidedness idl_members_excluded has, for the same reason.
   `why` is the sentence that says why the loop's filter cannot move a name across that partition unseen. See
   idl_args.c. */
void idl_install_constructs_column(JSContext *ctx, JSValueConst target, const char *const *column,
                                   int n, size_t stride, const char *const *refuses, int n_refuses,
                                   const char *why);

#endif
