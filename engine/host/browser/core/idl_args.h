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

/* WHAT A DECLARED TYPE ASKS OF UNKNOWN EXTERNAL INPUT — ONE statement of it, because it was TWO and they
   DISAGREED. The conversion stated it once as a pass-through's `t != … && t != … && t != …` chain and once as
   the ASSERT guarding §3.2.25's arm block, which restated it as "no concolic reaches here at all". Those are
   different sets: the chain lists the types the pass-through does NOT answer for, and three of them are types
   at which a concolic legitimately arrives — so the assert fired for every `any`-typed argument that was
   handed unknown external input (Indexed Database §4.5 The IDBObjectStore interface writes
   `IDBRequest put(any value, optional any key)`, which is two of them on one line) and named a union arm that
   value could not have reached, because the arms it names are all types the chain crossed.
   A hand-maintained mirror of a hand-maintained list is the defect, not either list; both readers ask THIS.
   The three answers are the three things a conversion can do to a value whose bytes it does not know. */
typedef enum {
    /* NOTHING IS ASKED AND NOTHING IS COERCED, so unknown external input is already what the body receives:
       `any` (no conversion at all by declaration — including every position past a non-variadic member's
       declared arity), §3.2.17's dictionary (not a value that crosses at all, but a bag of member READS, each
       a request like any other and each yielding another unknown), and §3.2.15's interface brand (whose only
       answer for a value that is not a platform object is a TypeError, and a TypeError de-taints nothing). */
    IDL_CONCOLIC_UNASKED = 0,
    /* THE CONVERSION COERCES — ToString, ToNumber, ToBoolean, §3.2.18's enumeration check. Opacity has to
       SURVIVE a coercion or the value stops forking control flow and stops being solvable at a sink, so the
       value CROSSES AS ITSELF and the body asks it for what it needs (concolic_shape_c for the bytes a Text
       node carries, the attribute taint shadow for a value parked in the DOM). It is the answer
       JSON.stringify gives an opaque field: yield the opaque, never a de-tainting placeholder. */
    IDL_CONCOLIC_CROSSES,
    /* THE CONVERSION'S ANSWER OVER UNKNOWN INPUT IS A SET OF FEASIBLE WORLDS THE MEMBER'S ALGORITHM TELLS
       APART — so it is neither crossed nor picked but FORKED, asked at the type's own resolution site so both
       worlds run. A type is only ever this when the SITE that resolves it asks that fork — the two assert
       against each other.
       TWO SHAPES REACH IT, THEY ARE NOT THE SAME QUESTION, AND THEY ASK DIFFERENT SEAMS — which is why this
       row is stated as the ANSWER and no longer as a §3.2.25 arm alone:
         - A UNION ARM THAT IS A TEST OF THE VALUE — Web IDL §3.2.25 Union types step 11 "If V is an Object"
           against step 12 "If V is a Boolean" and step 18 "If types includes boolean". No `if` a page writes
           asks that, so it is the machine asking which of its OWN completions it reaches: the OUTCOME seam,
           quickjs-step.h's step_fork_run, numbered by that site.
           AND NOT EVERY UNION WHOSE ARM TESTS THE VALUE IS ONE, WHICH IS THE HALF THIS SENTENCE DID NOT SAY
           AND WHICH A READER APPLIED AS WRITTEN. Taken literally it also covers `(DOMString or Function)`,
           `(Node or DOMString)` and `(object or DOMString)`, and those three are deliberately CROSSES with a
           paragraph each saying so — a criterion that does not separate the rows it governs from the rows it
           does not is a criterion nobody can check. THE DISCRIMINATOR IS WHETHER ONE PLACED VALUE CAN STAND
           FOR BOTH ARMS. Those three place the value ITSELF whichever arm is taken, so crossing an unknown
           loses nothing that was ever going to be computed here and the member's own algorithm still decides
           what to make of it — which is a fact about the member and not about the value, and is why the body
           is the only place that can. A union with a DICTIONARY arm cannot be crossed on those terms: step
           11.4 "If types includes a dictionary type, then return the result of converting V to that
           dictionary type" runs §3.2.17 Dictionary types' member WALK, and step 15 "If types includes a
           string type, then return the result of converting V to that type" runs no walk at all — so the two
           arms differ in what the conversion PERFORMS, and a value placed in the slot is already on exactly
           one of them. Cross it and the arm is still decided, just later and by whichever `JS_IsString` the
           body reaches first, from the SOLVER's value class rather than from the page's value.
         - Web IDL §3.2.3 boolean ITSELF, whose one step is ToBoolean and whose answer for a concolic is
           decided by ECMAScript §7.1.2 ToBoolean ( arg )'s last step ("Return true") rather than by anything
           about the page's value. That is the SAME PREDICATE `if (p)` asks, so it is the BRANCH seam —
           step_tobool_run — and `if (cfg.on)` and a member taking `cfg.on` are ONE gate with one constraint
           entry, one pin and one domain. Nothing is numbered, because a truth value has two completions and
           the branch seam computes which one a real session takes from the value's own example.
       Written as an ARM that is a test of the value, this row read as being about unions alone, and the
       boolean type sat under `default:` at CROSSES for as long as that wording stood — a type whose conversion
       DECIDES a world, filed with the types whose conversion merely coerces bytes a body still holds. */
    IDL_CONCOLIC_FORKS,
} IdlConcolicRule;

static inline IdlConcolicRule idl_concolic_rule(IdlArgType t)
{
    switch (t) {
    case IDL_ANY:
    case IDL_DICT:
    /* `D?` asks the value the same nothing `D` does — §3.2.20's null test reads no property and §3.2.17 is a
       bag of member READS, each a request like any other and each yielding another unknown. It is filed with
       IDL_DICT and never with the unions above, whose ARM is a test of the value. */
    case IDL_DICT_NULLABLE:
    /* A §3.6 LENGTH-DIFFERING SPLIT WHOSE TWO ENTRIES NEVER COEXIST AT ONE ARITY resolves from the argument
       count alone, so the conversion has already rewritten this position to the longer entry's number before
       any rule is asked — the only value the row itself describes is the dictionary at the shorter arity, and a
       dictionary asks the value nothing. See IDL_UNRESTRICTED_DOUBLE_OR_DICT. */
    case IDL_UNRESTRICTED_DOUBLE_OR_DICT:
    /* THE SAME ANSWER FOR THE SAME REASON WITH A USVString LONGER ARM — see IDL_USVSTRING_OR_DICT_BY_ARITY,
       whose two entries never coexist at one arity either, so the argument count has already rewritten this
       position before any rule is asked. It is NOT filed with IDL_USVSTRING_OR_DICT, whose entries DO meet and
       whose arm is therefore a test of the value. */
    case IDL_USVSTRING_OR_DICT_BY_ARITY:
    /* THE SAME ANSWER FOR THE SAME REASON, with the dictionary on the other entry: the arity has rewritten this
       position to one entry's own type before any rule is asked, so the pair is never the type of a value. */
    case IDL_CALLBACK_OR_DICT:
    /* A POSITION BEHIND THE DISTINGUISHING INDEX resolves from the RECORD of the entry that survived, which
       was settled before this position was reached — so the conversion has already rewritten this position to
       that entry's own type before any rule is asked, and what is left is a number (CROSSES on its own row) or
       a dictionary (a bag of member READS, each yielding another unknown). It is filed here for exactly
       IDL_UNRESTRICTED_DOUBLE_OR_DICT's reason and never with the unions above, whose ARM is a test of the
       value: this row tests nothing, and a FORKS rule here would be a second ask at a site with no second
       question — which the resolution site and this rule assert against each other. */
    case IDL_ULONG_OR_DICT_BY_ENTRY:
    /* A RECORD IS FILED WITH THE DICTIONARY ABOVE AND FOR THE SENTENCE ALREADY WRITTEN THERE: it is not a
       value that crosses at all but a bag of READS — §3.2.23's *convert a JavaScript value to record* step 3
       asks [[OwnPropertyKeys]], its step 4.1 asks each key's descriptor and its step 4.2.2 asks `Get(O, key)`
       — and each of those is a request like any other, each yielding another unknown. What separates it from a
       dictionary is only WHERE the key list comes from, which is not a question about the value's type.
       THE UNKNOWN-KEY-SET FORK IS NOT THIS TYPE'S AND MUST NOT BE DECLARED HERE. An enumeration of unknown
       external input forks over whether it holds an own member at all and then walks a per-position chain,
       each position asking whether there is a member beyond the ones already named; that ask belongs to
       step_ownkeys_run, which states the question in its own words there and which owns
       `keys_pred` and `keys_probe` for it, and it is asked once for every consumer of the request rather than
       once per declared type. Declaring FORKS here would be this conversion asking a second time, at a site
       with no second question to ask — and a type is only ever FORKS when the site that resolves it asks that
       fork, which the two assert against each other. */
    case IDL_RECORD_USVSTRING_STRING_OR_SEQUENCE:
    case IDL_INTERFACE:
        return IDL_CONCOLIC_UNASKED;
    /* `(AddEventListenerOptions or boolean)` — the one union of this shape in the DOM. Its arm decides
       whether DOM §2.7 "Interface EventTarget"'s flatten more options READS `once`, `passive` and `signal`
       off the value or leaves them at false, null and null, and a null `passive` is the whole of what makes a
       wheel listener on a Window passive by default — so the two arms differ in what the algorithm observes
       and neither may be picked for a value nothing is known about. */
    case IDL_DICT_OR_BOOL_FIRST:
    /* `(boolean or ScrollIntoViewOptions)` — the same fork for the same reason one row down: CSSOM VIEW §6's
       `scrollIntoView` step 6 makes the boolean arm's `false` set `block` to "end" where the dictionary arm
       leaves it at "start", and those are two different scroll positions rather than two spellings of one, so
       neither arm may be picked for a value nothing is known about. */
    case IDL_BOOL_OR_DICT:
    /* `(DOMString or D)` where D is a DICTIONARY — the same union one arm over, and it sat under `default:`
       at CROSSES for exactly as long as the row above did and for the same reason. §3.2.25 step 11 "If V is
       an Object" sends every Object down the dictionary arm and step 15 sends everything else to the string
       one; a concolic wears an ordinary Object, so the arm was DECIDED for every unknown external input by a
       fact about this engine's own value class.
       CROSSING IS NOT THE CURE HERE, WHICH IS WHAT SEPARATES THIS UNION FROM THE THREE THAT STAY AT CROSSES:
       the dictionary arm RUNS §3.2.17's member walk and the string arm does not, so the placed value is on
       one arm whatever a body asks of it next. What crossing actually bought was a concolic in the slot that
       every body of this type tests with `JS_IsString`, and a concolic fails that test by construction — so
       `new Sanitizer(cfg.preset)` reached HTML §8.6.2 The Sanitizer interface's `configure` and was
       canonicalized as a CONFIGURATION, and `document.createElement("div", cfg.opts)` reached DOM §4.5
       Interface Document's create-element step 3 as an options dictionary. Neither is a wrong test in its
       body; both are the arm being chosen where the arm is not knowable.
       BOTH ARMS ARE FEASIBLE AND THE ALGORITHMS TELL THEM APART: a preset name is Web IDL §3.2.18
       Enumeration types' check against `SanitizerPresets` and a configuration is a nine-member walk, and
       §4.5's `is` is read on one arm and not on the other. So neither may be picked for a value nothing is
       known about.
       OUTCOME 0 IS THE DICTIONARY ARM, per step_fork_run's one rule on the numbering — outcome 0 is what a
       run with no forking policy takes, and it is also the arm §3.2.25 gives the Object an unknown is
       represented BY, so a no-policy run answers exactly as it did and the STRING world is the one the fork
       adds. §3.2.25 step 4 "If V is null or undefined" is not part of the fork: null and undefined are real
       values a concolic is not, and they take the dictionary arm as they always did. */
    case IDL_STRING_OR_DICT:
    /* THE §3.6 SPLIT ONE ROW OVER, at the arity where step 4 removed NEITHER entry — the same two conversions
       the union above chooses between, chosen by a different algorithm. Web IDL §3.6 Overload resolution
       algorithm step 12.11 ("Otherwise: if V is an Object and there is an entry in S that has one of the
       following types at position i of its type list, a callback interface type a dictionary type a record
       type object … then remove from S all other entries") names the dictionary entry for ANY Object and step
       12.15 sends everything else to the string one, so a concolic — which wears an ordinary Object in this
       engine — had the entry chosen for it by a fact about the SOLVER's value class.
       IT IS THE SAME DISCRIMINATOR AND NOT A SECOND POLICY: the surviving entry decides which conversion RUNS
       — §3.2.17 Dictionary types' member walk against §3.2.12 USVString's scalar value conversion — so no
       single placed value is on both, and crossing merely moved the choice into whichever `JS_IsString` the
       body reached first. THE ARITY IS WHAT SCOPES IT. Where the longer entry SURVIVED, §3.6 steps 3-4 have
       already rewritten this position to that entry's USVString before any rule is asked (see
       idl_split_longer_type), so this row describes only the arity at which both entries stand and the value
       is what tells them apart — which is exactly the arity IDL_UNRESTRICTED_DOUBLE_OR_DICT never has, and
       why that row is UNASKED where this one forks.
       OUTCOME 0 IS THE DICTIONARY ENTRY, per step_fork_run's rule that outcome 0 is what a run with no forking
       policy takes — it is the entry step 12.11 gives the Object an unknown is represented BY, so a no-policy
       run answers exactly as it did and the USVString world is the one the fork ADDS. */
    case IDL_USVSTRING_OR_DICT:
    /* THE §3.6 SPLIT WHOSE TWO ENTRIES ARE THE SAME LENGTH, so step 4 removes neither at any arity and the
       whole decision is step 12's clause chain reading the value. It is the row above's answer over a THIRD
       feasible world: neither entry declares a string, numeric, boolean, bigint or `any` type at the split
       position, so no clause between step 12.11's dictionary and the end of the chain names an entry and step
       12.20's "Otherwise: throw a TypeError" is a world the standard reaches — `port.postMessage(m, "x")`
       throws where the same call on a Window names a target origin.
       THE THROW IS THE WORLD A TWO-ARMED FORK DROPS SILENTLY, because unknown external input wears an ordinary
       Object in this engine and every test at the resolution site is written over that Object. Its outcomes are
       (0) the dictionary entry, (1) the `sequence<object>` entry, (2) step 12.20's TypeError — see the site,
       which is where the ask is, and which is what makes the sequence world's own §3.2.21-over-unknown gap a
       named crash rather than an arm nobody chose. */
    case IDL_SEQUENCE_OBJECT_OR_DICT:
    /* `(BufferSource or D)` — §3.2.25 over Web Cryptography API §14.3.9 "The importKey method"'s `keyData`,
       and the union whose arms are furthest apart in what the conversion PERFORMS: the dictionary arm runs
       §3.2.17's whole member walk and the buffer arm reads no property at all. A concolic wears an ordinary
       Object in this engine, so step 11 "If V is an Object" claimed EVERY unknown external input for the
       dictionary — the arm decided by a fact about this engine's value class rather than by the page's value,
       which is the collapse the three unions above are here to prevent.
       THREE WORLDS AND NOT TWO, which is what separates this row from `(dictionary or boolean)` one line up.
       That union has a boolean arm and step 12 catches every non-object, so its clause chain never runs out;
       this one names no string, numeric, boolean or bigint type, so a real primitive that is not null or
       undefined reaches §3.2.25's OWN step 20 "Throw a TypeError" — `importKey("jwk", "abc", …)` throws where
       `importKey("jwk", {…}, …)` converts. A two-armed fork would drop that world silently.
       ITS OUTCOMES ARE (0) the dictionary arm, (1) the buffer-source arm, (2) step 20's TypeError. Outcome 0
       is the dictionary per step_fork_run's one rule on the numbering — it is the arm §3.2.25 gives the Object
       an unknown is represented BY, so a run with no forking policy answers exactly as it did and the other
       two worlds are what the fork ADDS. Outcome 1 is a world this engine cannot yet execute and says so at
       the site with a named crash rather than being quietly not chosen: §3.2.26 Buffer source types' "get a
       copy of the bytes held by the buffer source" has no answer over an unknown, because an unknown has no
       bytes. */
    case IDL_BUFFERSOURCE_OR_DICT:
    /* Web IDL §3.2.3 boolean — the type whose CONVERSION is the fork, where the two rows above are unions
       whose ARM is. ECMAScript §7.1.2 ToBoolean ( arg )'s last step is "Return true" and a concolic wears an
       ordinary Object, so a crossing
       boolean is not an unconverted value a body still holds: it is a value every `JS_ToBool` in every body
       answers `true` for, which is the collapse crossing exists to prevent, arriving one type below the union
       that had it. Both truth values are feasible and the algorithms behind this boundary observe different
       worlds for them — `cloneNode(deep)` copies a subtree or does not, `open(m, u, async)` is a synchronous
       XHR or an asynchronous one, `toggle(t, force)` adds a class or removes it — so neither may be picked.
       BOTH BOUNDARIES ANSWER IT AND THEY ANSWER IT AT THE SAME SEAM, which is why this is one row. §3.2.3 is
       reached from two places — an ARGUMENT position and a §3.2.17 dictionary MEMBER — and for a while only
       the first of them forked: the member loop crossed every unknown member as itself, so the pin merely
       MOVED from the conversion into whatever `JS_ToBool` the body used, and idl_dict_bool had to refuse the
       value to stop it. Crossing is not the cure for this type at either boundary, for the reason the
       paragraph above gives: a boolean's only consumer is control flow, so a crossed one has nowhere to go.
       Both sites ask step_tobool_run — the BRANCH seam — so a page that writes `if (cfg.on)`, passes `cfg.on`
       to a member and writes `{on: cfg.on}` files ONE constraint entry rather than three that can contradict
       each other.
       IDL_BOOLEAN_NO_DEFAULT IS THE SAME TYPE ASKING THE SAME QUESTION, and is here for that reason alone: it
       differs from IDL_BOOLEAN in what an ABSENT member means (see its declaration), which is a fact about
       `undefined` and says nothing about what §3.2.3 does with a value that is present and unknown. It sat
       under `default:` at CROSSES while IDL_BOOLEAN was already here — so on one dictionary
       (MutationObserverInit) four members were pinned to `true` by the readers the other two had stopped
       being pinned by. */
    case IDL_BOOLEAN:
    case IDL_BOOLEAN_NO_DEFAULT:
    /* Web IDL §3.2.18 Enumeration types — the SECOND type whose own conversion is the fork, and it is here for
       the boolean's reason reached by a different route. §3.2.3 forks because ToBoolean has two completions and
       a representation decides them; Web IDL §3.2.18 Enumeration types forks because its DOMAIN IS FINITE AND
       DECLARED: "If S is not one of E's enumeration values, then throw a TypeError" is the whole of what a
       value may be, so the worlds an unknown stands for are the N strings the IDL wrote plus that one refusal —
       N+1 completions, enumerable from the declaration alone.
       CROSSING IS NOT THE CURE, AND THAT IS WHAT MOVED THIS ROW. A crossed DOMSTRING reaches a body that asks
       it for its bytes and carries the taint to a sink; a crossed ENUMERATION reaches a body that was promised
       one of N strings and got an ordinary Object, so it either aborts on it or answers from this engine's
       value class — the collapse merely relocated, exactly as it was for the boolean. This type sat under
       `default:` at CROSSES for as long as that reading of the row above it stood, and the reading was that
       FORKS is about a union whose ARM is a test of the value: an enumeration has no arms in that sense, so
       nothing here named it.
       THE ARMS ARE ITS MEMBERS AS THE PAGE CAN TELL THEM APART, PLUS THE REFUSAL — never one per non-member
       string. §3.2.18 has ONE throw, and the strings that reach it differ in nothing the algorithm behind this
       boundary observes; two arms a page cannot tell apart are one world twice. The members themselves ARE told
       apart, and by the part of this project that most depends on it: Fetch's `credentials` is
       "omit" / "same-origin" / "include", and picking one for an unknown decides whether a request carries the
       person's cookies. OUTCOME 0 IS THE FIRST VALUE THE DECLARATION LISTS, per step_fork_run's one rule on the
       numbering — it is an ORDINARY completion of §3.2.18 rather than its throw, which is what that rule is
       about, and WHICH member it is comes from the IDL's own list order and from no ranking made here. An
       algorithm that then refuses the string it got (Fetch §5.4 step 17 refuses a "navigate" mode) is that
       algorithm's step and not this conversion's exceptional arm.
       BOTH BOUNDARIES ANSWER IT AND THEY ANSWER IT AT THE SAME SEAM, exactly as §3.2.3's two do — an ARGUMENT
       position and a §3.2.17 dictionary MEMBER, one statement in idl_enum_fork. It is the OUTCOME seam and not
       the branch seam: which of the conversion's OWN completions this position reaches is not a predicate any
       `if` the page writes asks, which is the discriminator quickjs-step.h states at both.
       ITS NULLABLE TWIN IS NOT HERE, and that is a stated residual rather than an oversight: `E?` has one world
       more (§3.2.20's null) and therefore a different ask — see IDL_ENUM_NULLABLE, which names what its absence
       shows. */
    case IDL_ENUM:
    /* §3.6's surviving OVERLOAD ENTRY at a distinguishing index whose two types are a NUMBER and a TYPED ARRAY
       — the same fork the two `…_OR_DICT` rows above ask, at the one position where the entries differ before
       the shorter one ends. Crossing is not the cure for the reason it is not theirs: the entry decides which
       CONVERSION runs at every later position of the call, so no single crossed value is on both, and the
       choice would merely move into whichever `JS_GetTypedArrayType` the resolution reached first — which
       answers "not a typed array" for a concolic by construction, so the numeric entry was being picked for
       every unknown by a fact about this engine's value class rather than by the page's value.
       TWO OUTCOMES AND NOT THREE, which is the row's own paragraph: step 12.20's TypeError is unreachable
       here because the chain's numeric fallback always names the shorter entry. Both worlds are real and the
       algorithms behind them observe different things — one reads `sw`/`sh` and allocates transparent black,
       the other takes the page's own buffer as the bitmap — so neither may be picked for a value nothing is
       known about. */
    case IDL_ULONG_OR_IMAGE_DATA_ARRAY:
        return IDL_CONCOLIC_FORKS;
    default:
        return IDL_CONCOLIC_CROSSES;
    }
}

/* WHICH DECLARED TYPES ASK FOR Web IDL §3.2.15 Interface types' BRAND — "If V implements I, then return … Throw
 * a TypeError", whose `I` a declaration has to state or there is nothing to test against.
 *
 * IT IS ONE STATEMENT BECAUSE IT WAS FIVE. The set lived as the `t ==` chain of each conversion arm that reads
 * a brand plus the DCHECK standing over each of them, and a set written once per reader is the second copy
 * CLAUDE.md names — the one that drifts is the copy nobody runs against reality, and idl_concolic_rule directly
 * above is here for exactly that reason and in exactly this shape. Every reader asks THIS: the three conversion
 * arms, idl_arg_iface's position check, and the seal's sweep over the whole platform, so a type added to the
 * enum that needs a brand is a type all five learn about at once.
 *
 * IDL_INTERFACE_NULLABLE and IDL_SEQUENCE_INTERFACE_NULLABLE are here even though §3.2.20's null rule collapses
 * them to their un-nullable type before any brand is read: the DECLARATION is what the seal and idl_arg_iface
 * see, and a `T?` position whose brand was never stated is a position whose non-null values reach §3.2.15 with
 * nothing to test. The `?` decides whether null is admitted, never whether an interface was named. */
static inline bool idl_type_brands_interface(IdlArgType t)
{
    switch (t) {
    case IDL_INTERFACE:
    case IDL_INTERFACE_NULLABLE:
    case IDL_SEQUENCE_INTERFACE:
    case IDL_SEQUENCE_INTERFACE_NULLABLE:
    /* The union's ARM is the brand test itself — `(Node or DOMString)` picks the object arm exactly when the
       value implements the interface — so a declaration with no brand cannot even choose an arm. */
    case IDL_STRING_UNLESS_IFACE:
    /* `(double or T)`, for the same sentence: §3.2.25's interface clause IS this union's arm, so a declaration
       with no brand has nothing to ask and every value would take the numeric arm — including the
       CSSNumericValue the member exists to receive. */
    case IDL_DOUBLE_UNLESS_IFACE:
        return true;
    default:
        return false;
    }
}

/* WHICH DECLARED TYPES ASK FOR Web IDL §3.2.18 Enumeration types' VALUE LIST — "If S is not one of E's
 * enumeration values, then throw a TypeError", whose `E` a declaration has to state or there is nothing to
 * test against. It is the same sentence idl_type_brands_interface directly above answers for §3.2.15's `I`,
 * one axis over, and it is ONE statement here for the same reason that one is: the set lived as each
 * conversion arm's `t ==` chain plus the DCHECK standing over it, and a set written once per reader is the
 * second copy CLAUDE.md names.
 *
 * Every reader asks THIS: idl_arg_enum's position check, the seal's sweep over every declared argument
 * position, and the seal's sweep over every declared DICTIONARY MEMBER — of both roads a member list is
 * recorded by, a member's anonymous dictionary argument and the intern table of named declarations — so a type
 * added to the enum that needs a value list is a type all of them learn about at once.
 *
 * THE CONVERSIONS ARE NOT AMONG THEM AND THAT IS DELIBERATE. An arm converting a value has already resolved
 * the position to ONE type and asks for that type by name (`t == IDL_SEQUENCE_ENUM`), because what it needs to
 * know is which element conversion to run and not whether some type in a set would want a list. Asking this
 * predicate there would collapse the three rows onto one arm, and they are three different conversions: a bare
 * `E` tests the member's own string, an `E?` admits null first, and a `sequence<E>` tests each element inside
 * §3.2.21.1's repeat loop. The predicate answers a DECLARATION-TIME question — was the type given the `E` it
 * needs — and that is the only question with one answer for all three.
 *
 * IDL_ENUM_NULLABLE is here even though §3.2.20's null rule collapses it to IDL_ENUM before any membership
 * test is reached, for idl_type_brands_interface's own reason: the DECLARATION is what the seal and
 * idl_arg_enum see, and a `E?` position whose values were never stated is a position whose non-null values
 * reach §3.2.18 step 2 with nothing to be one of. The `?` decides whether null is admitted, never whether an
 * enumeration was named. */
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

/* WHICH DECLARED MEMBER TYPES PUSH A LEVEL onto §3.2.17's conversion stack — the ONE statement of it, because
 * it is read by TWO things that must agree or the conversion crashes on a budget nobody was wrong about.
 *
 * The DEPTH (idl_members_depth) is what a host sizes its IdlConvFrame block from, and idl_dict_walk_start
 * asserts the block against it; the member LOOP is what actually pushes. A type counted and not pushed wastes a
 * frame, which nothing notices — and a type PUSHED and not COUNTED is a `CHECK` failure on the first push, of a
 * budget the declaration computed as zero. That is exactly what a nested plain dictionary would have hit: the
 * count was written for `sequence<(DOMString or D)>` alone and the loop grew a second pushing type, so the two
 * lists would have drifted the moment either moved. Both readers ask THIS.
 *
 * Each of these names its dictionary beside the member (IdlDictMember::dict) — the union's second arm for the
 * sequence, the member's own type for the other two — which is what makes the count a walk of the DECLARED type
 * tree rather than of the page's data: the tree is finite and ends at its own leaves, so page data nesting
 * deeper does not make the conversion deeper. */
/* DOES THIS TYPE PUSH A FRAME THAT READS NO DICTIONARY — the SECOND of the two questions
   `idl_type_pushes_level` used to answer alone, split out here because a `record<K, V>` is exactly where the
   two diverge and one bit answering two questions is decided by the stricter one.
   THE TWO QUESTIONS. idl_seal_check_dict_members pairs "pushes a level" with "names a dictionary" in BOTH
   directions — a member whose type pushes one must name a member list for it, and one that names a list must
   push. A record pushes a FRAME and names no dictionary at all: its keys come from the page's own object, so
   there is no declared member list and never could be. Left in the predicate above it would have failed that
   seal at the declaration; left out of the DEPTH count it would have failed idl_conv_push's capacity CHECK at
   the first page that used one. So the depth counter asks BOTH predicates and the seal asks only the first.
   A RECORD COSTS EXACTLY ONE FRAME AND NOTHING UNDER IT, which is why this needs no recursion beside
   idl_members_depth's: the frame holds the key cursor and the value's sequence, and a record's VALUE type
   names no further conversion that pushes. A record whose value were itself a record or a dictionary would
   change that — and would be a new row here, which is where the depth would have to be counted. */
static inline bool idl_type_pushes_record(IdlArgType t)
{
    return t == IDL_RECORD_USVSTRING_STRING_OR_SEQUENCE;
}

static inline bool idl_type_pushes_level(IdlArgType t)
{
    switch (t) {
    case IDL_DICT:
    case IDL_DICT_NULLABLE:
    case IDL_SEQUENCE_STRING_OR_DICT:
    /* `sequence<D>` nests one for the same reason its union sibling above does, and for a reason the seal
       states from the other side: a member whose type pushes a level MUST name its dictionary and a member
       that names one MUST push a level. Leaving this out would have made every `sequence<D>` member fail that
       pair — it names `dict` and would push nothing — which is the seal doing its job and is why this is one
       predicate and not a list at each reader. */
    case IDL_SEQUENCE_DICT:
        return true;
    default:
        return false;
    }
}

/* A DICTIONARY MEMBER, as its IDL declares it: the name, the type of its value, and whether the IDL marks it
   `required` (an absent required member is a TypeError, and for a dictionary `undefined` IS absent). A member
   with no `required` written is optional, which is what leaving the field off an initialiser gives. */
/* `values` is the NULL-terminated §3.2.18 value list of a member whose type NAMES AN ENUMERATION — which is
   every type idl_type_admits_enumeration answers true for and no other, so it is the dictionary-member half of
   what idl_arg_enum states beside an argument position. It is what §3.2.18 step 2's membership test is against,
   whether the member is a bare `E` / `E?` or a `sequence<E>` whose ELEMENT conversion runs that same step, and
   the seal asserts both directions of the pair over every declared member list at once — so a list stated at a
   type that reads none, or a type that reads one and states none, is a crash at the seal rather than on
   whichever call first reaches the member.
   `level` is WHICH DICTIONARY IN THE INHERITANCE CHAIN declares the member — 0 for the LEAST DERIVED
   dictionary in the chain, counting UP to D itself, which therefore holds the HIGHEST level. §3.2.17 step 3
   is "in order from least to most derived", so ascending level IS that order; step 4 sorts each dictionary's
   own members lexicographically among themselves. `FilePropertyBag : BlobPropertyBag` reads endings, type,
   then lastModified — an order no single sorted list produces, because `lastModified` sorts before `type`.
   Stating the level is what lets the declaration express that AND still be checkable.
   THE COUNT IS FROM THE ROOT AND NOT FROM D, and the difference is not pedantic in this tree: this line used
   to say "0 for the most-derived one's BASE", which names D's IMMEDIATE base and is the same number only for
   a two-deep chain. `KeyboardEventInit : EventModifierInit : UIEventInit : EventInit` is four deep and is
   declared here (core/events/ui_event.h splices levels 0-2 and each derived dictionary appends its own at 3),
   so read the retired sentence literally and EventModifierInit's members take level 0 — which would place
   them before EventInit's and read the chain inside out.
   A LEVEL IS NEVER LEFT AT ZERO FOR "the members this dictionary happens to list": a table whose members come
   from two dictionaries and states one level for all of them PASSES idl_dict_order_check whenever the two
   orders coincide, which they do for most *EventInit — so the fact is encoded here or it is not encoded at
   all, and the day a member is added that sorts before an inherited one, the abort names a row order that was
   never the problem. */
/* §3.2.17 step 4.1.5's DEFAULT VALUE, which is a THIRD state beside "the page wrote it" and "it is absent": a
   member whose IDL writes `= …` EXISTS on the converted dictionary even when the page wrote nothing, carrying
   that value. HTML §8.6.3 is where the difference bites — `SanitizerElementNamespace`'s namespace defaults to
   the HTML namespace and `SanitizerAttributeNamespace`'s to null, and §8.6.2's canonicalize a sanitizer name
   ASSERTS both members exist because of it, so `allowElement({name:"p"})` allows an HTML <p> and
   `allowAttribute({name:"href"})` allows a null-namespace href. Only the two forms the platform declares are
   here; a member whose IDL writes a different one names its own arm rather than being squeezed into a string. */
typedef enum {
    IDL_DEFAULT_NONE = 0,   /* the IDL writes no `= …`: an absent member does not exist */
    IDL_DEFAULT_NULL,       /* `= null` */
    IDL_DEFAULT_STRING,     /* `= "…"`, the string `dflt_str` holds */
    /* `= 0`. Indexed Database §4.2's IDBVersionChangeEventInit writes it for `oldVersion`, and the difference
       from IDL_DEFAULT_NONE is the same one this enum's own comment draws: an absent member does not exist, so
       the reader would have to invent the zero — which is precisely the consumer-side default that cannot be
       told apart from a measurement. Declared, the conversion places it and the reader asserts it is there. */
    IDL_DEFAULT_ZERO,
    /* `= false`. The Console Standard §1.1.1's `assert(optional boolean condition = false, any... data)` writes
       it, and it is a row here for the same reason IDL_DEFAULT_ZERO is: ToBoolean(undefined) is false, so a
       member that let the absence stand would be indistinguishable from one whose default was declared — until
       the day the position's type changes and the two stop agreeing. Declared, the conversion PLACES a real
       `false` and a body reading argv[0] is reading the IDL's value rather than inventing it. */
    IDL_DEFAULT_FALSE,
    /* `= true`. HTML §4.12.5.1.2's `CanvasRenderingContext2DSettings` writes `boolean alpha = true`, and it is
       a row here rather than an absence the reader fills for the reason IDL_DEFAULT_FALSE is one: ToBoolean of
       an absent member is FALSE, which is the OPPOSITE of this default, so a member that let the absence stand
       would answer `getContextAttributes()["alpha"]` false for every `getContext("2d")` called with no
       options — the majority of them. Declared, §3.2.17 step 4.1.5 places a real `true` and the body reads the
       IDL's value rather than inventing it. */
    IDL_DEFAULT_TRUE,
    /* `= 1`. Streams §4.5.1 Interface definition's `ReadableStreamBYOBReaderReadOptions` writes
       `[EnforceRange] unsigned long long min = 1`, and it is a row here for the reason IDL_DEFAULT_ZERO is
       one rather than being folded into it: the two are different VALUES, and this member's whole algorithm
       branches on the difference — §4.5's read(view, options) step 4 is "If options["min"] is 0, return a
       promise rejected with a TypeError", so a zero placed where the IDL writes one turns every
       `reader.read(v)` into a rejection. An absent member does not exist at all, so a reader that filled the
       absence itself would be inventing the number; declared, §3.2.17 step 4.1.5 PLACES it and the reader
       asserts it is there. */
    IDL_DEFAULT_ONE,
} IdlDictDefault;

struct IdlDictDecl;

typedef struct {
    const char *name;
    IdlArgType  type;
    bool        required;
    const char *const *values;
    uint8_t     level;
    /* THE DICTIONARY ARM of an IDL_SEQUENCE_STRING_OR_DICT / IDL_STRING_OR_DICT member's union — half of what
       that type states, the way idl_iface_brand's class is half of an interface arm. NULL for every other. */
    const struct IdlDictDecl *dict;
    IdlDictDefault dflt;
    const char *dflt_str;
    /* THIS MEMBER'S OWN §3.2.15 INTERFACE CLASS, for a dictionary that declares MORE THAN ONE interface type.
       `idl_iface_brand` states ONE class per DECLARATION, which is everything a dictionary whose interface-typed
       members are all the same interface needs — StaticRangeInit's two are both Nodes, FormDataEventInit's one
       is a FormData — and it is exactly what HTML §7.2.6.10.1's NavigateEventInit walks past: its four are a
       NavigationDestination, an AbortSignal, a FormData and an Element, so one class per declaration would have
       branded `signal` against NavigationDestination and refused every correct construction.
       ZERO IS A STATEMENT AND NOT A HOLE: it says this dictionary states its interface once, at the declaration,
       and the conversion asserts that one of the two was stated rather than reading past a missing class. It is
       therefore not the `x || 0` §Consumer-defaults forbids — there is no producer that could have written it. */
    JSClassID   iface;
    /* AND THE NARROWING THAT CLASS CANNOT EXPRESS, ON THE SAME MEMBER — the per-member half of
       idl_iface_narrow, which the class alone made unreachable. `idl_member_iface` takes BOTH from the member
       when the member states its class, so a member stating its own class no longer silently loses the
       DECLARATION's narrowing along with the declaration's class: those are two statements and taking one
       could only ever have dropped the other. Every DOM node wrapper is ONE class, so `iface` set to
       `node_class_id()` says "a Node" and can say no more, while HTML §7.2.6.10.1 The NavigateEvent
       interface's `Element? sourceElement` says Element — and without this a Text node or a Document crossed
       as one. NULL is a STATEMENT, exactly as `iface`'s zero is: the class names the interface exactly, which
       is true of `FormData? formData` on that same dictionary and of every member whose interface is one
       class. It is read by the two arms that read `iface` and by nothing else. */
    bool      (*iface_narrow)(JSValueConst v);
    /* §3.2.15's `I` STATED AS A PREDICATE INSTEAD OF AS A CLASS — the DICTIONARY counterpart of idl_arg_iface,
       which states the same thing at an argument POSITION and has the same two halves (the test, and the
       identifier the TypeError names). The pair above and this one are TWO SPELLINGS OF ONE FACT and never two
       facts: a member states §3.2.15's `I` exactly once, which idl_seal_check_dict_members asserts over every
       declared member list at once, and idl_member_implements is the ONE resolution both spellings are read
       through — so there is no site at which they could answer differently.
       WHY THE SECOND SPELLING HAS TO EXIST, in the three shapes that reach it, because ONE of them looks like
       an accident of this engine and the other two are properties of the standards:
         - AN INTERFACE NO CLASS ID NAMES BECAUSE MANY CLASSES IMPLEMENT IT. `EventTarget` is implemented by
           every node wrapper, by a Window and by an XMLHttpRequest, so no class comparison and no narrowing of
           one class can be its brand — the test is a prototype-chain walk against the REALM's
           EventTarget.prototype, and a realm is a JSContext.
         - AN INTERFACE WHOSE INSTANCE IS THE REALM'S OWN GLOBAL. `Window` is what `window` hands a page, and
           asking whether a value IS this realm's global is a question about the realm.
         - AN INTERFACE WHOSE CLASS IS SHARED BY CONSTRUCTION. Every indexed interface in this platform is one
           core/idl_indexed.c object, so `JS_GetClassID` cannot tell a MediaList from a CSSRuleList; each such
           component brands on the private-Symbol own slot that HOLDS its collection (core/css/media_list.c),
           and reading an own slot takes a JSContext.
       A NARROWING CANNOT SERVE ANY OF THE THREE, which is why this is a field and not a wider `iface_narrow`:
       §3.2.15's test would still begin with a class comparison, and the first two shapes have no one class to
       compare against while the third's class is shared with every interface it must be told apart from.
       `iface_name` is the interface's IDL IDENTIFIER and is the SUBJECT of the TypeError §3.2.15 throws — the
       same half idl_arg_iface's second argument is, for the same reason: a page told only that "the declared
       interface" was not implemented learns nothing it did not already know. It must outlive the declaration,
       so every member passes a static. NULL for a member that states its class instead, where the phrase the
       message falls back to is idl_member_iface_subject's. */
    bool      (*iface_is)(JSContext *ctx, JSValueConst v);
    const char *iface_name;
} IdlDictMember;

/* A DECLARATION OF THIS STRUCT NAMES ITS §3.2.15 TAIL, and that is a rule rather than a style: the struct has
   gained fields more than once, so a POSITIONAL initializer that runs to the end silently re-aims every value
   after the next field added — and where the two neighbours are both pointers (as `iface_narrow` and
   `iface_is` are), it re-aims them with no diagnostic at all. A list that STOPS short of the tail is fine, and
   is what most declarations do; what must not happen is a list that reaches the tail positionally. */

/* A DICTIONARY, DECLARED — its member list in §3.2.17's read order, and the identifier its IDL gives it. A
   member's OWN dictionary argument is declared as the bare list (idl_method_id_dict); a NESTED one needs that
   list NAMED, because the type that reaches it is stated on the member that holds it and a conversion
   diagnostic has to be able to say which dictionary refused a value. */
typedef struct IdlDictDecl {
    const char          *name;
    const IdlDictMember *members;
    int                  n;
} IdlDictDecl;

/* ---- WEB IDL §3.2.17 Dictionary types, AS AN EMBEDDABLE WALK --------------------------------------------
 *
 * ONE MACHINE, TWO ENTRIES — never two machines. §3.2.17 Dictionary types' ES-to-IDL conversion (the FIRST of
 * that section's two sibling ordered lists; the second converts an IDL dictionary back to an Object, and a bare
 * sub-number here would name a step in either) is reached two ways in this engine, and the pair is the whole
 * reason this declaration exists rather than a second copy of the loop:
 *
 *   - AS A DECLARED ARGUMENT TYPE. `optional D options = {}` is an IDL_DICT position, and the argument machine
 *     converts it at the argument boundary before the member's own algorithm starts.
 *   - INSIDE AN ALGORITHM, where the spec converts a value it is HOLDING rather than one Web IDL handed it.
 *     Indexed Database §5.12 creating a request to retrieve multiple items is the first: `getAll` and
 *     `getAllKeys` declare their first argument `any`, and step 8's "is a potentially valid key range" branch is
 *     what decides whether step 9 reads it as an IDBGetAllOptions at all — so the conversion cannot happen at
 *     the boundary, because the boundary does not yet know it is a dictionary.
 *
 * A SECOND COPY IS THE DUAL SYSTEM this engine forbids by name, and the seam between two copies is where the
 * bugs would be: a member's [[Get]] is §3.2.17 (ES-to-IDL list) step 4.1.3.1's `? Get(jsDict, key)` — a getter
 * or a Proxy trap, so the page's code — and step 4.1.4.1's "converting jsMemberValue to an IDL value whose type
 * is the type member is declared to be of" is the page's code AGAIN, once per member type that coerces
 * (§3.2.4.6 unsigned long's ToNumber is a `valueOf`, §3.2.18 Enumeration types' is a ToString). An algorithm
 * that hand-rolled a trio of step_getprop_run calls would be a dictionary machine whose required-member rule,
 * whose §3.2.17 step 4.1.5 defaults and whose per-member coercions could each drift from this one's.
 *
 * SO THE WALK IS THE ARGUMENT MACHINE'S OWN CURSOR, LIFTED OUT OF IT. The argument machine embeds exactly one
 * and drives it through the same idl_dict_walk_run an algorithm calls; there is no argument-only path left for
 * the two to disagree across.
 *
 * IT NEEDS NO STAGE OF ITS OWN, which is what makes it embeddable in an algorithm at all. Every rest point it
 * has is a REQUEST (step_getprop_run, step_tostring_run, step_todouble_run, iter_cursor_run), and a request
 * parks and resumes AT ITS OWN CALL SITE with the hosting machine's stage unmoved — so an embedder adds a field
 * and a re-entry, never a stage block the way core/indexeddb/idb_key_range.h's walk needs one.
 *
 * THE HOST'S HEADER IS A PARAMETER AND NOT A FIELD, because the walk is a sub-algorithm of whichever machine
 * embeds it: the requests are issued through the HOST's JSStepHdr, and a walk holding one of its own would be a
 * second machine with a second identity for the driver to assert about. */

/* ---- ONE LEVEL OF §3.2.17, AND THE STACK OF THEM ----------------------------------------------------------
 *
 * §3.2.17 CONVERTS A MEMBER BY ITS OWN DECLARED TYPE — step 4.1.4.1 is "Let idlMemberValue be the result of
 * converting jsMemberValue to an IDL value whose type is the type member is declared to be of" — and that type
 * may be ANOTHER DICTIONARY, at which point the same section runs again over a different member list while the
 * outer one is still standing on the member that named it. So the conversion is a STACK OF LEVELS, and a level
 * is everything the member loop reads: which list, where in it, and what is in flight on the member it is on.
 *
 * IT IS A LEVEL AND NOT A RECURSION because every rest point in it is the PAGE'S CODE — step 4.1.3.1's
 * `? Get(jsDict, key)` is one accessor or Proxy trap away from a page loop, and so is each member's own
 * coercion — and a park has to be a RETURN. C recursion would put the outer level's members in a C activation
 * no snapshot can carry.
 *
 * THE LEVELS OF ONE WALK ARE ONE LOOP. This used to be two: idl_dict_walk_run's member loop and a SECOND,
 * WEAKER one inside the sequence frames, whose arms were DOMString, DOMString? and another such sequence and
 * which aborted on everything else — so `DOMRectInit`'s four `unrestricted double` members converted through
 * one road and refused through the other. Two copies of one section is the dual system this engine forbids by
 * name, and the seam between them is where its bugs were; there is one loop now and a level is what it runs on.
 */
typedef struct {
    JSValue   src;      /* jsDict — step 4.1.3.1 reads from it; undefined or null is step 4.1.2's "no object" */
    JSValue   out;      /* step 2's idlDict, as the object this engine represents one by (owned) */
    const IdlDictMember *members;
    const JSAtom        *atoms;   /* their names, interned when the dictionary was declared */
    const char *name;   /* the dictionary's IDL identifier, for a diagnostic; NULL for an anonymous one */
    int       n;
    /* §3.2.15 Interface types' BRAND for this level's interface-typed members, and the narrowing a class id
       cannot express — see idl_iface_brand / idl_iface_narrow. A member carrying its own (IdlDictMember::iface)
       overrides BOTH, taking its narrowing from IdlDictMember::iface_narrow: the class and the narrowing are
       two statements about one member, so a member that states its class states its narrowing too rather than
       inheriting a narrowing written for a different interface. Zero and NULL for a level with no
       interface-typed member, AND for every PUSHED level: a nested dictionary is reached through a member and
       not through a declaration, so it has no declaration-wide class to state and each of its interface-typed
       members names its own. The conversion asserts that rather than reading past a missing one. */
    JSClassID iface;
    bool    (*narrow)(JSValueConst v);
    int       mi;       /* THE RESUME POINT: the member being read */
    /* 0 = read the member (step 4.1.3.1), 3 = decide whether it is THERE (step 4.1.4's "If jsMemberValue is
       not undefined"), 1 = convert what was read (step 4.1.4.1), 2 = place it. `2` is what a PUSHED level
       returns to: its own step 5 hands this level the converted dictionary, and the member must then be
       placed without re-running the read or the conversion.
       `3` RUNS BETWEEN 0 AND 1 AND IS NUMBERED LAST BECAUSE 2 IS THE ONE A PUSHED LEVEL NAMES: a decision
       vector and a pushed frame both record what they were standing on, so renumbering the settled phases to
       put this one in sequence would give an old record a new meaning. It is a phase of its own rather than a
       tail of phase 0 because the presence question can FORK, and a resume from that fork must re-enter after
       the read (which would otherwise run the page's getter twice) and before any conversion that can park
       (whose outstanding answer the ask would release). */
    uint8_t   mphase;
    JSValue   mv;       /* the member's value between those phases (owned) */
    /* §3.2.21 Sequences' cursor and the list it fills, for a member whose type is one. It is ALSO what the
       argument machine uses for a sequence at an ARGUMENT position: Web IDL converts arguments strictly left to
       right, so an argument's sequence and a dictionary member's are never in flight at once, and one cursor is
       what makes that structural instead of a comment two copies could drift across. It is PER LEVEL because
       two levels genuinely can have one in flight at the same time — an outer member's sequence is what pushed
       the level whose own member is a second sequence. */
    IterCursor seq;
    JSValue    seq_list;
    uint32_t   seq_n;
    /* 0 = NOT STARTED, 1 = pull the next element, 2 = convert the one just pulled. "Not started" is a phase of
       its own rather than a null list, because a zeroed state's JSValue is the INTEGER 0 and not JS_UNDEFINED —
       JS_TAG_INT is 0 — so "have I built the list yet" read off the value is always "yes". */
    uint8_t    seq_phase;
    /* §3.2.25 Union types' arm for a `(DOMString or sequence<DOMString>)` member or argument, which is a resume
       point because the decision is `? GetMethod(V, %Symbol.iterator%)` — the page's code. */
    uint8_t    uni_phase;
} IdlDictLevel;

/* WHAT KIND OF THING A PUSHED FRAME IS CONVERTING — the two shapes a member's declared type can name that need
   a level of their own, and the ONLY thing that differs between them is what happens when that level's step 5
   is reached. A DICTIONARY frame's result is the member's value one level down; a SEQUENCE frame's result is
   ONE ELEMENT, which joins the list and is followed by the cursor's next pull. */
enum { IDL_FRAME_DICT = 0, IDL_FRAME_SEQUENCE, IDL_FRAME_RECORD };

/* ONE PUSHED LEVEL. For IDL_FRAME_DICT that is the whole of it — `lvl` is the nested dictionary being read.
 * For IDL_FRAME_SEQUENCE it is a `sequence<(DOMString or D)>`'s own iterator PLUS the D-dictionary the element
 * it is standing on is being converted as, which is `lvl` again: §3.2.21.1 Creating a sequence from an iterable
 * puts the element conversion INSIDE the repeat loop, so the element's own §3.2.17 is a level like any other
 * and the frame parks at the element it is on AT WHATEVER DEPTH. */
typedef struct {
    IdlDictLevel lvl;       /* the dictionary this frame is converting — its own, or the element it stands on */
    IterCursor  cur;        /* SEQUENCE only: the sequence's iterator, over `src` */
    JSValue     src;        /* SEQUENCE only: the value being iterated (owned) */
    JSValue     list;       /* SEQUENCE only: the elements converted so far (owned) */
    const IdlDictDecl *d;   /* SEQUENCE only: the element type's dictionary arm */
    /* SEQUENCE only: THE ELEMENT'S DECLARED TYPE, which is what decides whether the pull has an arm to take at
       all — `sequence<(DOMString or D)>` asks §3.2.25's clause chain of every element and `sequence<D>` asks
       nothing, because §3.2.17 is the whole of its element conversion. It is the TYPE and not a bit meaning
       "no union": a bit would answer one question and this answers the one question a third element type
       would also need answered, which is what the element IS. */
    IdlArgType  elem;
    /* RECORD only: §3.2.23's *convert a JavaScript value to record* AS A CURSOR, which is core/idl_iter.c's
       and is the SAME one Headers and URLSearchParams drive from their own step machines. It is held here
       rather than re-implemented because that algorithm is the standard's once: step 3's [[OwnPropertyKeys]],
       step 4.1's [[GetOwnProperty]] and step 4.2's enumerable test, and step 4.2.2's `Get(O, key)` are all
       requests, so the frame parks at whichever key it stands on at whatever depth. `lvl` carries the VALUE's
       own conversion beside it — §3.2.25's arm in `lvl.uni_phase` and the sequence arm in `lvl.seq*` — which
       is what a per-level cursor is for and is why no second frame is pushed for the value. */
    RecordCursor rec;
    uint32_t    n;          /* SEQUENCE: how many elements `list` holds. RECORD: how many PAIRS it holds */
    uint8_t     kind;       /* IDL_FRAME_DICT / IDL_FRAME_SEQUENCE / IDL_FRAME_RECORD */
    uint8_t     phase;      /* SEQUENCE and RECORD */
} IdlConvFrame;

/* §3.2.17 IN FLIGHT — the whole of what a park has to carry, and nothing the host can re-derive.
 *
 * THE FRAMES ARE NOT IN HERE AND THAT IS THE POINT. A deep fork BYTE-COPIES the hosting state and re-takes only
 * what its `visit` names, so a pointer stored here into that same block would survive the copy STILL AIMED AT
 * THE ORIGINAL — two flows converting into one frame stack, which is the defect the argument machine's own tail
 * comment names one level up. They are passed to every entry instead, so the host re-derives them from its own
 * layout on each re-entry and there is nothing stored to go stale. An inline array would be worse still: its
 * size would be a CEILING on how deeply the PLATFORM's declared types may nest.
 *
 * `members`/`atoms`/`iface`/`narrow` are borrowed and must outlive the walk, which every caller satisfies by
 * passing statics — a member's declaration owns its list for the life of the pool, and an algorithm's is a file
 * static. They are re-stated on the walk rather than re-read from a declaration because an ALGORITHM's
 * dictionary has no declaration to read from; that is the whole difference between the two entries. */
typedef struct {
    /* LEVEL ZERO — the dictionary the host asked for. It is a field and not a special case: the member loop
       runs on whichever level is on top, and this is the one at the bottom. */
    IdlDictLevel lvl;
    uint8_t    conv_sp;   /* how many IdlConvFrame frames are live; 0 = level zero is the one in flight */
    uint8_t    started;   /* the walk has a `src` and an `out`; 0 = nothing in flight, so a resume may start it */
    /* THE NAME OF THE FORK THIS CONVERSION IS ASKING — step_fork_run's `op` for §3.2.17 step 4.1.4's PRESENCE
       question over a member minted off an unknown source, step_tobool_run's for a §3.2.3 boolean member over
       unknown external input, and step_fork_run's again for a §3.2.18 enumeration over one. It is HERE and not
       a C local because the driver reads `JSStepHdr::fork_op` AFTER the machine has returned JS_STEP_FORK, by
       which time a local of the member loop is gone; and it is this struct's rather than the header's shared
       `len_op` because that buffer belongs to the length probe and two mechanisms sharing one scratch space is
       how one overwrites the other's outstanding question.
       ONE BUFFER IS ENOUGH FOR THE THREE ASKS BECAUSE ONE FORK IS IN FLIGHT: step_fork_ask refuses a second ask
       while the first one's operands are still on the header, and they are sequential — the presence question
       is settled at `mphase` 3 before the boolean arm at `mphase` 1 is reached, and an ARGUMENT position's
       enumeration ask cannot overlap a member's at all, because Web IDL converts arguments strictly left to
       right and this walk lives inside the argument machine's own state (which is why `seq` and `uni_phase` are
       shared the same way, and for the same reason). WHAT KEEPS THEM APART IS NOT THIS BUFFER BUT THE NAME EACH
       COMPOSES INTO IT: JSStepHdr::fork_ask_key is a content hash of the string, so the ask strings begin with
       different spec steps and name the member or the position they stand at, and no one answer can be consumed
       at another's call site. It is SCRATCH and carries nothing across a park — each ask is re-composed from the
       declaration on every entry, and the key is what survives to match the answer to the question, so a
       byte-copied clone that never reads this buffer's stale contents is correct by construction. */
    char       ask[160];
} IdlDictWalk;

/* INTERN a dictionary declaration's member names, once per runtime, and answer them. The atom must be live at
   both the request and the answer — step_getprop_run is handed it twice with a suspension in between — so it
   cannot be created per read, and the names are static strings, so one intern serves every conversion. It also
   runs §3.2.17's READ-ORDER check over the declaration (see idl_args.c), which is why an algorithm's dictionary
   goes through it rather than reaching for JS_NewAtom itself. Idempotent: a declaration already interned answers
   with the atoms it has. Call it from the component's per-agent init. */
const JSAtom *idl_dict_declare(JSContext *ctx, const IdlDictDecl *d);

/* BEGIN §3.2.17 (ES-to-IDL list) over `src`, which the CALLER has already brought past step 1: a value that is
 * neither an Object, undefined nor null is that step's TypeError, and the two callers throw it in two different
 * places (the argument machine at the position, an algorithm wherever its own branch sent the value here), so it
 * is asserted here rather than performed here.
 *
 * undefined and null ARE legal and are not a special case: step 4.1.2 makes every member's jsMemberValue
 * undefined, so the same member loop runs and yields a dictionary carrying every declared default. That is why
 * there is no second "default them all" loop — there was one, in the argument machine, and a dictionary with
 * both would be two answers to §3.2.17 step 4.1.5.
 *
 * `frames`/`frames_cap` are the nested-conversion stack; a dictionary whose declared types nest needs at least
 * idl_members_depth of them and this asserts it, so a caller that passed none for a type that needs some crashes
 * at the start rather than at the depth. NULL/0 is right for a dictionary NONE of whose members declares a type
 * that pushes a level — no nested dictionary (`D`, `D?`) and no `sequence<(DOMString or D)>` — which is nearly
 * all of them. The two numbers are ONE STATEMENT: idl_members_depth counts exactly the member types the loop
 * pushes for, both reading idl_type_pushes_level, so a type added to that predicate is counted and pushed at
 * once rather than being pushed against a budget that never grew.
 * Returns 0, or -1 with a throw live (the object could not be minted). */
int  idl_dict_walk_start(JSContext *ctx, IdlDictWalk *w, JSValueConst src,
                         const IdlDictMember *members, int n, const JSAtom *atoms, const char *name,
                         JSClassID iface, bool (*narrow)(JSValueConst v),
                         IdlConvFrame *frames, int frames_cap);

/* DRIVE the conversion one re-entry's worth. Returns >0 (the caller returns it — the walk is parked inside a
   member's [[Get]] or inside one member's own coercion), 0 when every member has been read and converted, or -1
   with a throw live. `in` is the request answer and is CONSUMED. */
int  idl_dict_walk_run(JSContext *ctx, JSStepHdr *hdr, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap,
                       JSValue in, JSValue **out_cb, int *out_argc);

/* TAKE step 5's idlDict, OWNED, and leave the walk empty — so a host that converts one dictionary per call may
   start another. Asserts the walk finished: taking a half-read dictionary would hand an algorithm an object
   whose absent members are indistinguishable from members the page did not write. */
JSValue idl_dict_walk_take(JSContext *ctx, IdlDictWalk *w);

/* WHAT THE WALK OWNS, for the hosting state's `visit` to chain into its own. The frames are visited here too —
   ALL of them and not only the live ones, because a popped frame holds JS_UNDEFINED and a never-used one the
   zeroed state's non-refcounted integer, so visiting all takes no reference it should not, where a loop bounded
   by `conv_sp` would silently drop whatever a frame still held if the cursor and the frames ever disagreed. */
void idl_dict_walk_visit(JSContext *ctx, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap, JSStepVisit *v);

/* RELEASE everything an ABANDONED walk holds and leave it empty — for a host whose own teardown is not the
   `visit`-driven discharge (an algorithm that owns its state directly). A host whose `visit` names the walk
   needs nothing here: the driver's one discharge covers it. Safe on a walk that never started. */
void idl_dict_walk_clear(JSContext *ctx, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap);

/* DECLARE a member: the IDL types of its arguments, and the body to run once they are converted. Returns the
   step id, which the caller CACHES. Registration and installation are separate on purpose: Element's members
   are installed on every wrapper the tree hands out, so registering there would mint a definition per element.
   A position the IDL does not list is passed through unconverted, which is what a variadic `any...` tail means
   and what an optional argument beyond the listed ones means. `nargs` is how many the IDL lists, and THERE IS
   NO CEILING ON IT. There was one — IDL_MAX_DECLARED, which sized an inline type array in the member record
   and a per-call argument array in the machine's state, "the same bound seen from the declaration side and the
   call side". It was four, then eight, and each time its comment named the widest member then written as
   though that were a fact about the platform: eight was HTML 9.4.1's `initMessageEvent`, and Pointer Events 4's
   `initMouseEvent` (fifteen) and UI Events §6.1.2's `initKeyboardEvent` (ten) were BOTH already past it, both
   absent from this engine, and both said so in a comment naming this line. A ceiling that decides which spec
   members may exist is a cap on the platform, and raising it only moves the next member that cannot ship.
   So the DECLARATION owns its type list (copied at registration, freed with the pool) and the state's argument
   vector is sized from the same number — a member is as wide as its IDL, and there is nothing left to outgrow.
   `types` is COPIED, so a caller may pass a stack array. */
int  idl_method_id(JSContext *ctx, const IdlArgType *types, int nargs, IdlBody body, int magic);

/* §3.2's INTEGER conversion, over the double a ToNumber has already produced — sign(x)·floor(|x|) taken modulo
   the type's width, folded into range if it is signed, with [Clamp] rounding half to even instead. Public
   because a conversion that happens OUTSIDE this machine needs the same arithmetic: Web IDL converts a
   callback's RETURN VALUE to the operation's declared type, and DOM §6.3's `acceptNode` returns an
   `unsigned short`, so a filter answering 65537 accepts exactly as one answering 1 does. Written a second time
   in the component that needed it, that is a modulo somebody has to remember. */
int64_t idl_integer_of(IdlArgType t, double x);

/* THE NUMBER A CONVERTED NUMERIC ARGUMENT DENOTES, for a body that needs a real one — the numeric twin of
   concolic_name_cstr, and it exists for the same reason that one does.
   A BODY MAY NOT CALL JS_ToFloat64 ON ITS OWN ARGUMENT. §3.2's conversion is a BOUNDARY, and unknown external
   input crosses a boundary AS ITSELF (see the pass-through in the conversion loop) so that opacity survives
   the coercion — so a numeric position reaches its body either as the Number the declaration produced or as
   the unknown, and JS_ToFloat64 on the second owes C a real number it cannot have. Every body that wrote
   `JS_ToFloat64(ctx, &d, argv[i])` under a comment saying "already converted by the declaration" therefore
   ABORTS on `f(x * n)` with an unknown operand, which is a page's ordinary arithmetic and not a broken
   invariant.
   FOR AN UNKNOWN IT ANSWERS THE REAL CONVERSION RUN ON THAT VALUE'S OWN EXAMPLE — §3.2.4.5's
   ConvertToInt(V, 32, "signed") over the concrete the code actually computed, through the one copy of that
   arithmetic above, never a rule predicting what it would have produced. The value itself stays unknown: this
   is the modelled NUMBER an engine algorithm needs, not a collapse of the value to it.
   RETURNS 0 WHEN THE UNKNOWN CARRIES NO EXAMPLE YET, which is a POSITIVE statement rather than a hole to
   default: there is no number to fall back to, choosing one would INVENT a value the code never computed, and
   what that absence means differs per member — so the CALLER answers it. */
int idl_number_of(JSContext *ctx, IdlArgType t, JSValueConst v, double *out);

/* §3.2.4.8's `unsigned long long`, as the MAGNITUDE rather than as the int64_t bit pattern the modulo leaves —
   the half of that type's range above 2**63 is exactly the half a page reaches by writing a negative, and an
   int64_t cannot express it. Public for the same reason idl_integer_of is: a conversion performed outside this
   machine (File System §2.5's write algorithm reads its dictionary members with its own request) must not own
   a second copy of the arithmetic. */
double  idl_unsigned_long_long_of(double x);

/* §3.2.11's ByteString RANGE over UTF-8 bytes: true when every code point is 0x00..0xFF. Public because a
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
   names. The state is zeroed before the first entry.
   `presult` is where it leaves the member's answer. */
typedef int (*IdlStepBody)(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc);

/* THE FIRST STAGE THAT IS THE MEMBER'S OWN. Stages 0 and 1 belong to the machine that hosts every declared
   member — the argument-count check and the ES-to-IDL conversions — and BOTH are rest points, because a page's
   `toString` runs inside the second one. A body's own algorithm therefore starts here, and it rests on
   `hdr->stage` rather than on a private counter of its own: a stage is where a machine parks, where a sibling
   overtakes it and where a cold-tier resume picks it up, and the driver asserts at do_step_step that the stage
   a machine holds is a step its declaration names. A private byte in the body's state is invisible to that
   assert, so a body keeping one has a resume point nothing can check and nothing can report.
   A body that has not been converted still keeps its own byte; it declares no steps and is not yet asked. */
#define IDL_STEP_FIRST 2

/* AND A MEMBER'S OWN X-LIST IS BASED HERE, not on the first entry of every list. quickjs-step.h's
   JS_STEP_STAGE_ENUM emits a bare `name,`, which numbers from zero — right for a machine that owns all of its
   stages, wrong for a declared member, whose first two belong to the prologue above. Writing `= IDL_STEP_FIRST`
   on each list's first entry would state that same fact once per member, which is the per-member line this file
   exists to remove; here it is stated once and every list is expanded the same way:

       enum { IDL_STEP_STAGE_BASE(QS_STAGES) QS_STAGES(JS_STEP_STAGE_ENUM) };

   `list` names the machine's X-list so two members in one file declare two distinct enumerators. C numbers an
   enumerator with no value as its predecessor + 1, which is the whole mechanism. */
#define IDL_STEP_STAGE_BASE(list) list##_base = IDL_STEP_FIRST - 1,

/* The state's OWNERSHIP contract. `visit` is the ONE declaration of what the state holds, and it has three
   consumers, none of which knows about the others: the deep-fork clone takes a second reference to each field,
   the teardown releases each (tramp_step_state_free_1 discharges it after idl_args_result has stated the
   member's completion), and idl_args_result's own assert folds it into a number to check that `release` did not
   touch it.
   THERE IS NO SECOND LIST. `release` used to be that — the same JSValues, by hand, in another function — and
   the pair is exactly what this engine forbids: adding a field to a state then creates an obligation in two
   places and nothing catches the one that is missed. It had already been missed, in querySelectorAll: `visit`
   named the collected-matches array, the teardown named nothing, and every abandoned selector walk leaked its
   element wrappers. */
typedef struct {
    IdlStepBody body;
    size_t      state_size;
    void      (*visit)(JSContext *ctx, void *state, JSStepVisit *v);
    /* WHAT THE DECLARATION CANNOT NAME, AND WHAT HOLDS NO REFERENCE — a lexbor handle, a foreign C allocation,
       a global or per-object FLAG the algorithm took and must give back on every exit (§4.13.4 step 14's
       "regardless of whether the above steps threw", HTML §4.10.22.4 "Constructing the entry list" step 8's
       give-back of the constructing-entry-list flag its step 2 took — §4.10.22.3's form-submission algorithm
       only READS that flag, at its step 2, and never sets it).
       It runs BEFORE the declaration is discharged, so it may READ an owned value — those flags live on one —
       and idl_args.c folds the declaration into a number on each side of the call and requires the two to
       agree. A member with nothing of that kind declares NULL.
       WHAT THAT FOLD MEASURES IS SLOT IDENTITY — every declared slot's tag, and its payload where that is a
       pointer — AND NEVER A REFERENCE COUNT, so read the rule as "leave every slot the declaration names naming
       the same thing" rather than "hold still". It folded the heap's count once and could not: a count states
       how many holders an object has and never which, so a give-back dropping ANOTHER holder's reference to an
       object a declared slot also names was indistinguishable there from a `release` discharging the
       declaration itself, and every completed `document.createElement` of a defined name aborted. A `release`
       is therefore NOT forbidden to move reference counts elsewhere in the agent's object graph; it is
       forbidden to free, null, replace or hand over one of these slots. §4.13.4's active custom element
       constructor map give-back is still declared to the machine instead — see idl_active_ctor_owed — because
       it is half of a PAIR that must unwind in nesting order below that bracket, which is a different reason
       and an independent one. */
    void      (*release)(JSContext *ctx, void *state);
    /* WHICH ALGORITHM THIS MEMBER IS, AND WHICH OF ITS STEPS EACH STAGE RESTS AT — the host half of
       JSTrampStepDef's own declaration, and it lands on the same field of the same definition: the pool builds
       one JSTrampStepDef per member, prepends the two labels for the stages it owns itself, and the driver's
       one check reads the result. So a member is asserted by exactly the mechanism a quickjs.c machine is,
       rather than by a second one written for the host.
       `steps` is indexed from IDL_STEP_FIRST and NULL-terminated: `steps[0]` is the step the body rests at on
       its first entry. The label is the standard's own wording ("DOM §4.4 step 3"), because the point of it is
       that a parked flow can SAY where it is parked and that the number means the same thing in the next
       session as in this one. A stage names ONE spec step, and what may share one is decided by the ENGINE and
       never by the page: quickjs-step.h's JSTrampStepDef::steps carries the rule and the reason — a boundary is
       a rest point because the engine may have to park there (RAM pressure, a cold-tier eviction, a
       cross-session resume, a flow that outranks this one), and none of those consult the page. A member may
       therefore name a RANGE only when the whole range is ONE O(1) engine action, and the label says the range
       in those terms; a span of the PAGE'S size is a stage per step, whose walking stage returns JS_STEP_YIELD
       at every turn. js_step_def_check refuses a label that argues from the page's code at all.
       Both or neither: a member declaring one without the other is half a declaration, which the pool refuses
       rather than accepting an algorithm with unnamed steps or steps belonging to no algorithm. */
    const char *algorithm;
    const char *const *steps;
    /* THIS MEMBER'S OWN ALGORITHM CATCHES AN ABRUPT REQUEST RESULT, instead of letting it propagate. The pool's
       definition always declares JSTrampStepDef::catches_abrupt — HTML §4.13.6 step 1.3.1 catches in the
       epilogue EVERY member ends through — so the abrupt arrives at this machine either way; this field says
       which of the two implementations handles it. Zero means the epilogue's, and the body never sees it: an
       argument coercion's throw and the body's own request re-raise exactly as they did before. One means the
       BODY's, and the body is then re-entered with JS_EXCEPTION at the request's call site with the throw
       still live — which is what DOM §4.9 step 5.1.4's "run these steps while catching any exceptions" is, and
       the only reason `document.createElement` can report a throwing custom element constructor instead of
       letting it destroy the document. A body that declares this MUST answer for the abrupt at every request
       it makes: re-issuing the request instead is an infinite re-ask, because a keyed read's own two-phase
       cursor is reset by the abrupt delivery. */
    uint8_t     catches_abrupt;
    /* WHY THIS MEMBER'S STATE MUST NOT BE FORKED RIGHT NOW — the reason, or NULL when it may be. Forwarded onto
       the pool's definition, so the fork asks the MEMBER through the same one door it asks everything else; see
       JSTrampStepDef.unforkable for the capability this restores and for why the question belongs at the fork
       rather than inside the member's `visit`. NULL for a member that may always be forked.
       WHICH MEMBERS ARE NOT NULL IS A DERIVATION AND NEVER A LIST HERE, BECAUSE THIS SENTENCE CARRIED ONE AND
       IT WENT WRONG IN THE DIRECTION NOTHING REPORTS. It read `which is every one of them but the FRAGMENT
       PARSE`, and that was an ENUMERATION OF AN ABSENCE: it asserts of every other member that it declares no
       reason, so a reader is told the trajectory below is one machine from zero. Measured against the tree it
       was false by three — the §5.6 fetch machine, the §5.4 Request constructor and §5.1's Headers constructor
       all declare one, each for the PARSED HEADER LIST and none for any parser — and the error is the
       UNDER-CLAIM §AN-UNDER-CLAIM-IS-NOT-FOUND-BY-ACTING-ON-IT names, found by nobody acting on it, because
       acting on it means not looking. Derive the set instead, with both forms, since this field is initialised
       positionally at some definitions and designated at others:
           git grep -nE '\.unforkable *= *[a-z_]' -- engine/
           git grep -nE '^ *[0-9]+, *[a-z_]*unforkable' -- engine/
       THE STRUCTURAL FACT, WHICH IS WHAT DOES NOT ROT: a declarer names a CAPABILITY and not a machine, so the
       count of declarers and the count of things to build are different numbers and the first is the larger.
       THAT FETCH TRIO IS THE WORKED EXAMPLE OF IT AND WAS MEASURED RATHER THAN ARGUED, AND IT IS NOW THE
       WORKED EXAMPLE OF THE CAPABILITY REACHING ZERO: the fetch machine's reason went from THREE terms to one
       to none — §2.2.5's request record became JSValues, §5.2's extracted body became a list core/fetch/body.h
       declares, and §5.4 step 33's header list became a root quickjs-step.h's `tree` operation copies and
       destroys through headers.h's `header_list_step_ops`. THREE capabilities built one at a time under ONE
       declarer, which no count of declarers could have predicted. The header list was the LAST and it was the
       same capability behind the §5.4 Request constructor's only term and behind core/fetch/headers.c's §5.1
       constructor, so all three retired in one diff; THAT third one declared no reason at all until it was
       found by sweeping for the capability rather than for the field, which is the direction this derivation is
       blind in: it lists the
       machines that DECLARE a reason and cannot list the machines that OWE one. So it is an upper bound on the
       work and a LOWER bound on the machines a capability touches, and a machine missing from it is not
       therefore safe — it is a fork TAKEN where its siblings refuse one.
       Two machines holding one missing capability retire together and neither names it twice — core/frame/
       navigable.c's load says so of itself and points at the fragment parse's own list — so read each reason
       for WHAT IT WOULD TAKE and group by that before pricing any of it. The field goes when every one of those
       is built, and §A-superseded-system-is-DELETED is why it may not become anything else in the meantime. */
    const char *(*unforkable)(const void *state);
} IdlStepDecl;
/* DECLARE WHERE THE OPTIONAL ARGUMENTS START. §3.6 makes an `undefined` passed for an optional argument with
   no default mean the argument is ABSENT — `new URL("aaa:b", undefined)` is a one-argument call, and
   converting that undefined would give the base URL the string "undefined" and throw. Set after the
   declaration — it names the member the LAST one made, the way idl_method_id_ext sets `variadic`, because the
   id a declaration returns is the RUNTIME's step id and not this pool's index. A member that never calls this
   converts every declared position, which is right for a member whose arguments are all required.
   IT IS AN INDEX INTO THE MEMBER'S OWN LIST, and its "there are none" value is `nargs` — one past the last
   position the member declares, which is the only place "no optional arguments" can mean anything. It used to
   be IDL_MAX_DECLARED + 1, a sentinel derived from the CEILING: a member could name a position past what it
   declared and past what any member may declare, and the value that meant "none" changed whenever the ceiling
   did. The declaration asserts the bound, so the state cannot be reached. */
void idl_optional_from(int first_optional);

/* §3.6's "IF X IS GIVEN", WHICH IS THE QUESTION A SPEC STEP ASKS AND `argc` IS NOT AN ANSWER TO.
 *
 * Web IDL §3.6 "Overload resolution algorithm" outputs `values`, and its step 9 says what that list holds:
 * "Initialize values to be an empty list, where each entry will be either an IDL value or the special value
 * 'missing'". Two kinds of entry — and a spec step written "If src is given" is asking WHICH KIND is at that
 * position, never how many entries there are. Both of the algorithm's arms produce the second kind:
 *
 *   - step 15.4, for a position the page REACHED — "If optionality is 'optional' and V is undefined, then: If
 *     the argument at index i is declared with a default value, then append to values that default value.
 *     Otherwise, append to values the special value 'missing'."
 *   - step 16.2, for a position it never reached — "Otherwise, if callable's argument at index i is not
 *     variadic, then append to values the special value 'missing'."
 *
 * (For a member with ONE entry in its effective overload set — every member of this platform that is not a
 * length-differing split — step 8 never runs, so `d` stays −1, step 11's loop never executes and step 15's
 * does. The same sentence appears at 11.4 and 15.4; the one this platform reaches is 15.4.)
 *
 * WHY THE COUNT IS THE WRONG INSTRUMENT, IN BOTH DIRECTIONS. It over-reports, because a page that PASSES
 * `undefined` at an optional position with no default has raised the count for an argument §3.6 calls
 * missing — `new Audio(undefined)` is a one-argument call whose `src` is not given. And it over-reports
 * again from the other side, because the count this machine hands a body is EXTENDED over every defaulted or
 * dictionary position behind the ones the page passed (step 16.1's placement), so a member with a default
 * anywhere after the position in question reaches its body at full arity for every call: `Option`'s count is
 * 4 whether or not `value` was given. A count answers "how far did the page reach", and the spec is asking
 * "is there a value here".
 *
 * THE CONTRACT. Ask this of an OPTIONAL position — one at or past the member's `idl_optional_from` index.
 * A required position is always given (§3.6 step 5 threw otherwise), so the question is not one the standard
 * poses there, and an `any` at a required position legitimately holds the `undefined` the page passed. A
 * position with a DECLARED DEFAULT is always given too, and answers so: the default was placed, and no
 * IdlDictDefault produces `undefined`.
 *
 * WHAT KEEPS IT TRUE. `undefined` in the vector IS the representation of "missing", and the machine asserts
 * both directions of that — at the placement, that step 15.4.2's arm placed nothing else; and at the body
 * boundary, that no conversion handed an optional position an IDL value that is `undefined`. A body may read
 * the vector directly for the same answer (`!JS_IsUndefined(argv[i])` is what this computes); it exists so
 * the site states the SPEC'S question rather than restating its encoding, and so the day "missing" needs a
 * representation that is not `undefined` there is one place to change. */
bool idl_arg_given(int argc, JSValueConst *argv, int index);

/* DECLARE WHERE THE **LONGER OVERLOAD ENTRY'S** OPTIONAL ARGUMENTS START — the other half of a §3.6 split whose
 * two entries differ in LENGTH (IDL_USVSTRING_OR_DICT, IDL_UNRESTRICTED_DOUBLE_OR_DICT), and the half this file
 * stated as a rule and then applied to exactly one position.
 *
 * §3.6 step 15.3 reads optionality "at index i in the list of optionality values of the REMAINING entry", and
 * IDL_USVSTRING_OR_DICT's own paragraph above says so in those words — but only the SPLIT POSITION was ever
 * resolved that way. Every position AFTER it went on being measured against the DECLARATION's
 * `first_optional`, which is the SHORTER entry's, because the declaration has one number and §3.6 needs one
 * per surviving entry. Nothing had noticed, and the reason is worth stating rather than being lucky twice:
 * HTML §7.2.2's `postMessage` is the only member that had ever declared such a split, and its third argument
 * (`optional sequence<object> transfer = []`) is optional in the LONGER entry too — so the one number happened
 * to be right for both.
 *
 * CSSOM VIEW §6's `scroll(unrestricted double x, unrestricted double y)` is where they disagree, and it
 * disagrees at the ordinary case rather than at an edge: the declaration MUST make position 0 optional, because
 * the dictionary entry writes `optional ScrollToOptions options = {}` and `el.scrollTo()` is a legal call — and
 * position 1 is REQUIRED in the entry that survives at arity 2. Without this, `el.scrollTo(1, undefined)` reads
 * position 1 as an ABSENT optional and the body is handed nothing where §3.2.8 owes it ToNumber(undefined),
 * which §3.2's normalize-non-finite then makes 0. One number, two entries, and the wrong one silently wins.
 *
 * It names the member the LAST declaration made, as idl_optional_from and idl_arg_default do, and it must be
 * stated for EVERY member declaring a length-differing split: idl_args_seal walks the platform and asserts it,
 * so a member that forgot cannot reach a conversion. */
void idl_overload_split_optional_from(int longer_first_optional);

/* DECLARE A §3.6 LENGTH-DIFFERING SPLIT WHOSE TWO ENTRIES SHARE THEIR TYPE AT THE SPLIT — the case the type
 * list cannot state, and which the machinery above could not express because the only two members that had
 * ever needed one also changed TYPE there.
 *
 * THEY ARE TWO FACTS AND THIS SEPARATES THEM. §3.6 steps 3-4 remove entries by ARGUMENT COUNT, and the position
 * that removal turns on is the last one the SHORTER entry declares; §3.6 step 15.2's "let T be the type at
 * index i in the type list of the remaining entry" is a different question, and it only has a second answer
 * where the two entries' type lists differ there. IDL_USVSTRING_OR_DICT and IDL_UNRESTRICTED_DOUBLE_OR_DICT
 * answer both at once, which is why the position was READ off the type list — correct for them, and an
 * expressibility hole for every overload distinguished by arity alone.
 *
 * CSS Conditional Rules 3 §7.5's `supports` is the member that needs it: `supports(CSSOMString property,
 * CSSOMString value)` and `supports(CSSOMString conditionText)` are one shared prefix, one type, and two
 * lengths. Without this the shorter entry's optional index governs at every arity, so §3.6 step 15.4.2 makes
 * position 1 "missing" for `CSS.supports("(width:1px)", undefined)` — a call whose argument count is 2, whose
 * surviving entry requires position 1, and which must therefore convert that `undefined` to the string. The
 * one-argument reading of it answers TRUE where the two-argument reading answers FALSE, so it is not a corner:
 * it is the same wrong-entry-wins defect idl_overload_split_optional_from was written for, one declaration
 * further out.
 *
 * `shorter_last_position` IS THE LAST POSITION THE SHORTER ENTRY DECLARES, which is what `split_at` means for a
 * type-declared split too — the union type sits AT the shorter entry's final index. It names the member the
 * LAST declaration made, as idl_optional_from does, and it must be stated BEFORE
 * idl_overload_split_optional_from, which asserts a split exists to describe. A member whose type list already
 * names a split may not also state one here: two answers to "which count removes an entry" is a member whose
 * every arity is resolved by whichever was found first.
 *
 * **-1 IS A SHORTER ENTRY THAT DECLARES NOTHING, AND IT IS NOT A SENTINEL.** This refused anything below 0
 * under a sentence claiming the shorter entry "DECLARES AT LEAST ONE POSITION" — which was never derived from
 * the standard: Web IDL §2.5.8 Overloading's own worked example for an effective overload set contains
 * `(f3, « », « »)`, and §3.6 step 4's removal "by argument count" has nothing to say about a count of zero.
 * The bound was true of every member that had been declared and it was read as a rule. HTML §4.10.7 "The
 * select element" is where it breaks: `[CEReactions] undefined remove();` and `[CEReactions] undefined
 * remove(long index);` are ONE identifier with two entries, the shorter declaring no argument at all, and the
 * whole of what distinguishes them is arity — §4.10.7 says so in prose rather than by type, "when it has
 * arguments" against "when it has no arguments". So the shorter entry's final index is the position BEFORE
 * the list, exactly as idl_optional_from's "there are none" is the position PAST it.
 * IT COST A SEPARATE `has_split` FIELD, WHICH IS THE §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS SPLIT: `split_at`
 * was answering both "is there a split" and "where", the two agreed for as long as no shorter entry was
 * empty, and the stricter question owned the sentinel. */
void idl_overload_length_split_at(int shorter_last_position);

/* DECLARE §3.6's DISTINGUISHING ARGUMENT INDEX — the position step 12 chooses the surviving overload entry at,
 * which is a DIFFERENT number from `split_at` and was the same one for every member declared before HTML
 * §8.11.1 "The ImageData interface".
 *
 * `split_at` is where the SHORTER entry ENDS, which is what §3.6 steps 3-4 remove an entry by. `d` is where the
 * entries' types first DIFFER, which is what step 12 reads a value at. Web IDL §2.5.8 Overloading requires the
 * two to coincide only in one direction — "for each index j, where j is less than the distinguishing argument
 * index …, the types at index j in all of the items' type lists must be the same" bounds what may differ
 * BEFORE `d` and says nothing about what lies between `d` and the shorter entry's end. Window's `postMessage`
 * differs at index 1 and ends at index 1; ImageData differs at index 0 and ends at index 2, and every assert
 * this file wrote over `split_at` as if it were `d` is an assert about the first shape only.
 *
 * IT IS READ OFF THE TYPE LIST WHEREVER THE TYPE LIST CAN SAY IT, exactly as `split_at` is: a value-resolved
 * split row AT a position states the position and both entries' types at once, so asking for the number again
 * would be one fact stated twice and free to disagree. This declaration exists for the member whose value split
 * is at a position its own type list cannot name — and there is none yet, which is why it asserts that the
 * declaration loop already found one rather than setting a number beside it.
 * RETIREMENT: this record goes when `d` and `split_at` can no longer be read as one number, which is when no
 * assert in this file names `split_at` in a sentence about which ENTRY survived. */
void idl_overload_distinguishing_at(int d);

/* WEB IDL §3.6 Overload resolution algorithm's DEFAULT VALUE AT A POSITIONAL ARGUMENT — the THIRD state at a
   position, beside "the page passed one" and "the argument is absent", and exactly the distinction
   IdlDictDefault already draws for a dictionary member. §3.6's absent rule above is for an optional argument
   with NO default value; where the IDL writes `= …`, the algorithm places THAT value and the body never sees
   a hole.

   THE CONVENTION FOR CITING IT, STATED ONCE HERE AND NOT RE-DERIVED AT EACH SITE — §3.6 is 17 top-level steps
   and it places a declared default in TWO of them, which is why no single sub-number names this rule:
     - step 15.4.1, inside `While i < argcount` — the page REACHED the position and passed `undefined` there.
       Its guard is step 15.4, "If optionality is 'optional' and V is undefined", and its sibling 15.4.2 is the
       absent rule ("append to values the special value 'missing'").
     - step 16.1, inside `While i is less than the number of arguments callable is declared to take` — the page
       STOPPED SHORT of the position. Its sibling 16.2 appends "missing" only "if callable's argument at index
       i is not variadic", which is what makes a variadic member's declared head behave like any other.
   A site that means the rule cites both; a site that means only one of the two cites that one. Step 11.4.1 is
   the third such clause and is deliberately NOT cited anywhere: it sits in `While i < d`, which runs only when
   step 8 set a distinguishing argument index, and step 8 sets one only "if there is more than one entry in S"
   — a length-differing split has been reduced to one entry by steps 3-4 before then, so this engine cannot
   reach it. The day a same-length overload is declared, that clause becomes reachable and this list grows.
   IT WAS CITED AS "step 14.2" AT TWENTY-TWO SITES AND THAT NUMBER IS A REAL STEP ABOUT SOMETHING ELSE: step 14
   is "If i = d and method is not undefined", the arm that builds a sequence from an iterator, and its 14.2 is
   "Let T be the type at index i in the type list of the remaining entry in S". A reader who followed it landed
   on a live step that mentions no default at all, which is the failure mode a wrong number has and a missing
   one does not.
   IT WAS NOT EXPRESSIBLE AND THE BODIES PAID FOR IT. Indexed Database §4.4's
   `transaction(storeNames, optional IDBTransactionMode mode = "readonly", …)` is the member that needs it: with
   only "absent" to say, the body would read `undefined` and substitute "readonly" itself — the IDL's own
   declaration re-derived in a body, which is the consumer-side default §Offensive-programming names, and the
   next member declared that way would re-derive it again with nothing to keep the two equal.
   The default is already an IDL value — it is written in the IDL and not computed from the page — so it is
   PLACED and never coerced, and no enumeration check runs over it. Set after the declaration, naming the member
   the LAST one made, exactly as idl_optional_from does; the position must be one that declaration listed and
   must already be optional, both of which are asserted. `dflt_str` must outlive the declaration. */
void idl_arg_default(int index, IdlDictDefault dflt, const char *dflt_str);

/* DECLARE THE CLASS AN IDL_INTERFACE / IDL_STRING_UNLESS_IFACE POSITION BRANDS AGAINST. Set after the
   declaration, naming the member the LAST one made, exactly as idl_optional_from does and for the same reason:
   the id a declaration returns is the RUNTIME's step id and not this pool's index. It composes with every
   declaration form — a method, a setter, a step body — which idl_method_id_ext's `iface` parameter did not. */
void idl_iface_brand(JSClassID iface);

/* NARROW an IDL_INTERFACE position past what a CLASS can express. Every DOM node wrapper is one class, so
   `idl_iface_brand(node_class_id())` says "a Node" and cannot say "an Element", "an HTMLElement" or "an
   HTMLFormElement" — and the platform's IDL says all three. §4.13.7.3's `optional HTMLElement anchor` is where
   that first mattered: `setValidity(flags, msg, document.createElementNS('some-ns','foo'))` must be a
   TypeError and a class check crosses it as itself.
   The predicate runs AFTER the class check and its failure is the same TypeError, so a member declares the
   interface it means in ONE place rather than repeating a hand-written test in its body — which is the whole
   reason the brand is part of the type. Set after the declaration, naming the member the LAST one made, as
   idl_iface_brand and idl_optional_from do.
   IT IS THE DECLARATION-WIDE FORM AND A DICTIONARY MEMBER HAS ITS OWN — IdlDictMember::iface_narrow, beside
   that member's own class. A dictionary whose interface-typed members are all one interface states both here,
   once; one that declares several (NavigateEventInit's four) cannot, because this names ONE predicate for the
   whole declaration and a `FormData` is not an `Element`. */
void idl_iface_narrow(bool (*is)(JSValueConst v));

/* DECLARE §3.2.15's `I` AT ONE POSITION — "If V implements I, then return … Throw a TypeError" — for a member
 * the two declarations above cannot describe. It OVERRIDES them at the position it names and at no other, which
 * is the same shape and the same reason IdlDictMember::iface has: one statement per declaration is everything a
 * member whose interface-typed positions are all one interface needs, and the members that walk past it need
 * the fact to be about the POSITION.
 *
 * TWO SHAPES OF MEMBER NEED IT AND THEY ARE DIFFERENT PROBLEMS.
 *   - MORE THAN ONE INTERFACE IN ONE ARGUMENT LIST. Pointer Events 4 §16.1 Initializers for interface
 *     MouseEvent declares `initMouseEvent(… optional Window? viewArg = null, … optional EventTarget?
 *     relatedTargetArg = null)` — fifteen positions, two interfaces, and one brand per declaration can name at
 *     most one of them. Declared IDL_ANY instead, BOTH positions crossed unconverted and the body ran the two
 *     conversions by hand, which is the brand test written out in a body that a declared type exists to
 *     replace.
 *   - AN INTERFACE NO CLASS ID NAMES. §3.2.15's word is "implements", and idl_iface_brand's `JSClassID` answers
 *     it only for an interface whose values are exactly one class. `EventTarget` is implemented by every Node,
 *     every Window, every MessagePort, every AbortSignal and every `new EventTarget()`; `Window` is the
 *     realm's own global OR a WindowProxy (see core/frame/window_proxy.h, which states why those are one type
 *     test and not two). For those the class-plus-narrowing pair has no class to start from — the narrowing
 *     runs AFTER a class check that has already refused the value — so the whole test has to be the predicate,
 *     which is the same conclusion idl_this_iface below reaches for the receiver and for the same sentence of
 *     the spec.
 *
 * IT TAKES A JSContext AND idl_iface_narrow DOES NOT, because an interface reached through a PROTOTYPE CHAIN is
 * a per-realm fact: "does this object implement EventTarget" is answered by looking for THIS realm's
 * EventTarget.prototype on its chain, and a predicate with no realm would have to reach for a remembered one —
 * the one-fact-answered-from-one-place defect CLAUDE.md names. The ctx the conversion passes is the MEMBER's
 * realm (js_call_c_function sets it), which is the realm whose interface object the call went through.
 *
 * `iface` is the interface's IDL identifier and it is NOT decoration: it is the subject of the TypeError, so a
 * page that passes the wrong thing is told which interface it failed rather than that "the declared interface"
 * was not implemented. It must outlive the declaration, so every caller passes a static.
 *
 * Set after the declaration, naming the member the LAST one made, exactly as idl_iface_brand and
 * idl_optional_from do. The position must be one the declaration listed and its type must be one
 * idl_type_brands_interface answers true for, both asserted here; idl_args_seal asserts the other direction
 * over the whole platform — every branding position has a brand, stated here or by idl_iface_brand. */
void idl_arg_iface(int index, bool (*is)(JSContext *ctx, JSValueConst v), const char *iface);

/* DECLARE THE INTERFACE THIS MEMBER'S *RECEIVER* MUST IMPLEMENT — Web IDL §3.7 Interfaces' implementation-check
 * an object, step 3: "If object does not implement interface, then throw a TypeError."
 *
 * THE TWO BRANDS ARE DIFFERENT QUESTIONS AND THIS IS THE OTHER ONE. idl_iface_brand above states what an
 * ARGUMENT position admits; this states what `this` must be. They are declared side by side because they read
 * alike and they are answered at opposite ends of the member: §3.7.7 Operations' create an operation function
 * asks the receiver's in its try-list's step 2.1.2.3, BEFORE step 2.1.4 computes the effective overload set and
 * therefore before §3.6 Overload resolution algorithm converts one argument, while an argument's own brand is
 * part of that conversion.
 *
 * WHICH IS WHY THE RECEIVER'S BRAND CANNOT LIVE IN A BODY. A member's body runs after every conversion, so a
 * brand test written there lets `Iface.prototype.member.call({}, { toString() { … } })` run the page's
 * `toString` and only then throw — where a browser throws with nothing of the page's code having run. The order
 * is observable, so it is the spec's and not a convenience.
 *
 * THE PREDICATE IS THE WHOLE OF THE TEST, unlike idl_iface_brand's class-plus-narrowing pair, and the reason is
 * §3.7.6 Attributes / §3.7.7 Operations' word "implement": a member declared on Element is reached on an
 * HTMLDivElement, whose wrapper carries a DIFFERENT class id, so a class comparison answers the wrong question
 * for every interface anything inherits from. The component that owns the interface already states the right
 * one (its `…_is` predicate), so this names that rather than restating it.
 *
 * `iface` is the interface's IDL identifier, used only to say which interface the TypeError is about; it must
 * outlive the declaration, so every caller passes a static. Set after the declaration, naming the member the
 * LAST one made, exactly as idl_iface_brand and idl_optional_from do and for the same reason. */
void idl_this_iface(bool (*is)(JSValueConst v), const char *iface);

/* DECLARE §3.2.18's `E` AT ONE POSITION — the enumeration whose value list IS the type, as the
 * NULL-terminated array of the identifiers the IDL lists. §3.2.18 step 2 is "If S is not one of E's
 * enumeration values, then throw a TypeError", so the conversion checks the string ToString produced against
 * this and refuses anything else: `history.scrollRestoration = "bogus"` is a TypeError from the TYPE, before
 * the setter's algorithm runs at all, and a body performing it would be one body's private copy of a rule
 * every enumeration member has.
 *
 * IT IS PER POSITION, and that is the whole of what this states beyond the values. It was one list per
 * DECLARATION, which is everything a member whose enumeration positions are all one enumeration needs — and
 * Web Cryptography §14.3.9 The importKey method is the member that walks past it:
 *
 *     Promise<CryptoKey> importKey(KeyFormat format, BufferSource keyData, AlgorithmIdentifier algorithm,
 *                                  boolean extractable, sequence<KeyUsage> keyUsages);
 *
 * TWO enumerations on one line — §14.1 Data Types' KeyFormat at position 0 and KeyUsage as the ELEMENT type
 * at position 4 — so one list per declaration could name at most one of them, and the second was checked by
 * hand in the member's body. That is the brand test written out in a body which a declared type exists to
 * replace, and it is the identical shape idl_arg_iface answers for §3.2.15's `I`.
 *
 * SO THERE IS NO DECLARATION-WIDE FORM TO FALL BACK TO. A per-position list subsumes it exactly — the
 * declaration-wide one was this call at whichever position asked — where idl_iface_brand survives beside
 * idl_arg_iface because a CLASS and a PREDICATE are two different tests. Keeping both here would be one fact
 * stated two ways with a fallback between them, which is the dual system CLAUDE.md forbids, so the old form is
 * gone and every caller names its index.
 *
 * Set after the declaration, naming the member the LAST one made, exactly as idl_arg_iface and
 * idl_optional_from do. The position must be one the declaration listed and its type must be one
 * idl_type_admits_enumeration answers true for, both asserted here; idl_args_seal asserts the other direction
 * over the whole platform — every position whose type admits an enumeration has one. `values` must outlive the
 * declaration, so every caller passes a static, and IDL_ENUM_VALUES below is how one is written. */
void idl_arg_enum(int index, const char *const *values);

/* DEFINE A §3.2.18 VALUE LIST — AND SUPPLY ITS TERMINATOR, so it cannot be left off.
 *
 * Both readers of a value list scan it for a NULL: the positional conversion behind idl_arg_enum and the
 * dictionary member that names the list in an IdlDictMember row. Neither can bound the scan, because both
 * receive a POINTER and a pointer has already lost the extent — so the list's length lives entirely in its own
 * last element, and until now nothing about writing one made that element mandatory. A list missing it is not
 * a list that reads short; it is a scan that walks off the end of the array into whatever the link placed
 * after it, which is undefined, and which on this engine's shipping target does not fault (see check.h's
 * pointer-invariant note). The compiler is then entitled to assume the walk cannot happen and to conclude the
 * loop cannot exit, so the symptom is a HANG that presents as slowness rather than a fault at the wrong
 * declaration.
 * SUPPLYING THE TERMINATOR REMOVES THE POSSIBILITY RATHER THAN REPORTING IT, which is why this is the primary
 * mechanism and check.h's DCHECK_SENTINEL is only for a list some other macro did not declare. It also means
 * the value lists say what the IDL says and nothing else — the terminator is this engine's own bookkeeping and
 * never part of the enumeration §3.2.18 defines.
 * THE EXTENT IS DELIBERATELY LEFT UNWRITTEN (`[]`). A hand-written extent is a second copy of the list's length
 * and the two go out of sync in the one direction that is silent: an `extern T x[N]` whose definition supplies
 * MORE than N entries is truncated to N with only a warning, and the entry truncation drops is the LAST one —
 * the terminator. Writing the bound by hand to make a scan safe is therefore how the terminator goes missing.
 * A list shared across translation units uses the EXTERN form and declares `extern const char *const name[];`
 * in its header, incomplete, so no second copy of the length can exist to drift. */
#define IDL_ENUM_VALUES(name, ...)        static const char *const name[] = { __VA_ARGS__, NULL }
#define IDL_ENUM_VALUES_EXTERN(name, ...)        const char *const name[] = { __VA_ARGS__, NULL }

/* DECLARE WHICH OF §3.2.26 Buffer source types' TWELVE TYPED ARRAYS AN IDL_TYPED_ARRAY POSITION IS, and which
   of §3.3's two buffer extended attributes the IDL writes on it.
   §3.2.26 step 1 is "let T be the IDL type V is being converted to" and step 2 tests [[TypedArrayName]]
   "with a value equal to T's name", so the conversion cannot START without T — which is also why this is
   stated per POSITION and not per member: a member may declare several, and Web Audio API §1.13.3 Methods'
   `getFrequencyResponse(Float32Array frequencyHz, Float32Array magResponse, Float32Array phaseResponse)` on
   the BiquadFilterNode interface is three of them on one line. The index is into the member's own type list,
   exactly as idl_arg_default's is.
   `allow_shared` is §3.3.2 [AllowShared] and `allow_resizable` is §3.3.1 [AllowResizable], read straight off
   the IDL, because they are the CONDITIONS §3.2.26 steps 3 and 4 turn on: a position carrying neither refuses
   a SharedArrayBuffer-backed view AND a resizable-buffer-backed one, and Encoding §7.4's `[AllowShared]
   Uint8Array destination` refuses only the second. They are two independent flags because §3.3.1 and §3.3.2
   are two independent attributes — §3.3.2's own example writes all four combinations — so collapsing them into
   one "kind of buffer position" loses two of the four.
   Set after the declaration, naming the member the LAST one made, exactly as idl_arg_default, idl_iface_brand
   and idl_arg_enum do. idl_args_seal asserts BOTH directions: a position declared IDL_TYPED_ARRAY that
   states no T is a conversion that cannot start, and a T stated at a position of any other type is a
   declaration describing a member that is not this one. */
void idl_typed_array(int index, JSTypedArrayEnum kind, bool allow_shared, bool allow_resizable);

/* DECLARE THAT THIS MEMBER'S TAIL IS VARIADIC — `T... name`, so the LAST declared type applies to every
   argument from that position on and the member takes as many as the page passed.
   IT IS SET AFTER THE DECLARATION, naming the member the LAST one made, exactly as idl_optional_from,
   idl_arg_default, idl_iface_brand and idl_arg_enum do. It existed only as a parameter of
   `idl_method_id_ext`, which builds a PLAIN-BODY member — so a member that is BOTH a step machine and variadic
   could not be declared at all, and the Console Standard's namespace is nine of them (`log(any... data)` and
   its eight siblings reach §2.2's Formatter, which calls the page's `toString`). A flag that composes with
   every declaration form is the same answer this file already gave for the brand and the enumeration list. */
void idl_variadic(void);

/* DECLARE THAT THIS MEMBER'S IDL RETURN TYPE IS A PROMISE — Web IDL §3.7.7's create an operation function,
 * whose `Try` wraps the brand check, the overload resolution, EVERY argument conversion and the method steps,
 * and whose last steps are: "if an exception E was thrown: If op has a return type that is a promise type,
 * then return ! Call(%Promise.reject%, %Promise%, «E»). Otherwise, end these steps and allow the exception to
 * propagate."
 *
 * SO IT IS A DECLARATION AND NOT A BODY'S JOB. `crypto.subtle.digest('SHA-256', {})` REJECTS — a page that
 * wrote only `.catch` around it is relying on that, and a member that threw instead would take the whole flow
 * down at a call site the bundle believed it had covered. Before this the only way to get it was to declare
 * every argument `IDL_ANY`, call idl_optional_from(0) so the arity check could not throw, and re-derive each
 * argument's type inside the body — a hand-written brand test per member, which is exactly what the type list
 * above exists to have one of.
 *
 * Set AFTER the declaration, naming the member the LAST one made, exactly as idl_optional_from, idl_arg_default,
 * idl_iface_brand, idl_arg_enum and idl_variadic do, and for the same reason: the id a declaration returns
 * is the RUNTIME's step id and not this pool's index. It composes with every declaration form. */
void idl_returns_promise(void);

int idl_method_id_step(JSContext *ctx, const IdlArgType *types, int nargs,
                       const IdlDictMember *members, int nmembers,
                       const IdlStepDecl *decl, int magic);

/* THE MAGIC THIS INVOCATION WAS DECLARED WITH. A plain body takes it as an argument; a step body cannot, because
   its signature is the step contract and that is shared with every machine in the engine. It is read off the
   header instead, which is the same place the receiver and the arguments come from — one declaration serving two
   members (innerHTML and outerHTML are one walk with two starting points) is exactly what a magic is for. */
int idl_step_magic(const JSStepHdr *hdr);

/* DECLARE THAT THIS INVOCATION ENTERED §4.13.4'S ACTIVE CUSTOM ELEMENT CONSTRUCTOR MAP, so the machine gives
 * the entry back at its teardown. `ctor` is DOM §4.9 create an element step 5.1.1's `C`, BORROWED — the machine
 * takes its own reference and drops it when it leaves.
 *
 * WHY A MEMBER CANNOT SIMPLY DO THIS IN ITS `release`, which is where every other give-back on this machine
 * lives. Steps 5.1.5-5.1.6 must run at the exit a discarded flow takes — it is parked on the page's
 * constructor and no resume ever comes back — so the teardown is the only place they CAN run; and this half of
 * the pair has to leave in NESTING ORDER with the other half, §4.13.5 "Upgrades" step 10's regardless-list,
 * which custom_elements_queue_unlock pays below idl_args.c's `release` bracket. A member's `release` runs
 * BEFORE that unlock, so an upgrade reached from inside this member's own Construct would leave the OUTER
 * bracket first. This is the door that puts DOM §4.9's bracket exactly where §4.13.5's already was — below the
 * bracket, in the same teardown, unwound in nesting order.
 *
 * IT IS NOT THE FINGERPRINT THAT FORCES IT, THOUGH IT ONCE WAS, and the correction is recorded here because the
 * next reader will otherwise re-derive the old reason. The map is a map of constructors to registries, so
 * giving an entry back drops a reference to `C`, a value the member's own `visit` names for the whole bracket;
 * idl_args.c used to fold the HEAP's count of every declared value and therefore refused this give-back and a
 * `release` discharging the declaration alike, aborting every completed `document.createElement` of a defined
 * name. That fold now reads slot IDENTITY and no count, so it would no longer object. The ordering above is
 * what keeps this door, and it stands on its own.
 *
 * ONE PER INVOCATION, asserted: the pair nests one deep per declared member, because the one bracket a member
 * enters directly brackets a single Construct. */
void idl_active_ctor_owed(JSContext *ctx, JSStepHdr *hdr, JSValueConst ctor);

/* READ a member of the dictionary the declaration built. An `optional D options = {}` argument that the page
   did not pass is not there at all, so a body that reads it with JS_GetPropertyStr calls a property get on
   `undefined` — a pending TypeError, and a truthy JS_EXCEPTION where a `false` belonged. That is a mistake per
   BODY, which is the thing this machine exists to have only one of, so reading a dictionary is part of the
   declaration's contract: an absent dictionary has every member absent, and that is all it means.
   Nothing of the page's is on the object these read, so neither runs any of its code. */
JSValue idl_dict_get(JSContext *ctx, JSValueConst dict, const char *name);
/* THE BOOLEAN READ CARRIES ITS CALLER'S ADDRESS, because its assert is one line reached from every dictionary
   in the platform, and a crash stamped `idl_args.c` names a defect with no object. The pair is captured at the
   CALL and never derived here, which is why this is a macro expanded at each site and not a second function
   wrapping the first; the member's own name travels with it because one name (`bubbles`, `capture`) is
   declared by many dictionaries and only the pair says which read it was.
   WHAT IT NOW REPORTS IS AN OBJECT OR A DECLARATION AND NO LONGER A MISSING FORK ANYWHERE. §3.2.3's fork is
   performed by the member loop (idl_concolic_rule answers IDL_CONCOLIC_FORKS for both boolean types, so the
   crossing does not rewrite them and the loop asks step_tobool_run), and the loop also decides §3.2.17 step
   4.1.4's PRESENCE at the outcome seam before that — which is what closed the one case that used to leave a
   NO-DEFAULT boolean crossing as itself. So a member read off a converted dictionary arrives as a real truth
   value or as an absence. An unknown here says one of two things — the object never went through §3.2.17, or
   the member is not declared a boolean — and the refusal itself is where that split is stated. */
bool    idl_dict_bool_at(JSContext *ctx, JSValueConst dict, const char *name,
                         const char *file, int line);
#define idl_dict_bool(ctx, dict, name) idl_dict_bool_at((ctx), (dict), (name), __FILE__, __LINE__)

/* THE TWO ACCOUNTS A HAND-ROLLED MEMBER READ OWES, WHICH ARE NOT ONE DEFECT AND MUST NOT SHARE A MESSAGE.
   A body that reads a member with idl_dict_get and then asserts its SHAPE is asserting that §3.2.17
   (ES-to-IDL list) step 4.1.4.1 converted it — "Let idlMemberValue be the result of converting jsMemberValue
   to an IDL value whose type is the type member is declared to be of". That assertion has TWO ways to fail
   and only ONE of them is a conversion that went wrong.
   THE FIRST IS THE CROSSING, AND IT IS THE ENGINE WORKING AS DESIGNED. idl_dict_walk_run's member loop
   rewrites a CONCOLIC member's declared type to IDL_ANY BEFORE any type arm is asked — for every declared
   type whose rule is not IDL_CONCOLIC_FORKS, which is every type this macro is used at (the one exception is
   §3.2.3's boolean, whose conversion IS the fork and which therefore reaches its arm and is placed as a real
   truth value; idl_dict_bool is the reader for those, and it makes the opposite assertion) — so unknown
   external input crosses the boundary AS ITSELF and reaches the body still wearing the Object
   solver/concolic.c gives it. Nothing was converted, so nothing failed. A shape assert phrased "reached this
   body unconverted" is then a CORRECT CRASH WITH A FALSE EXPLANATION — the worst shape an assert has, because
   the crash IS right and only its account is wrong, so it does not announce itself. A reader who obeys it
   goes looking for a conversion that failed, finds a declaration doing exactly what it was written to do, and
   is left with two conclusions the tree cannot afford: weaken the assert, or rebuild the coercion the
   crossing exists to prevent.
   THE SECOND IS THE ORIGINAL INVARIANT, UNCHANGED: a value that is neither the declared type's output nor
   unknown input means the member is not declared that type in the IdlDictMember list the operation registered.
   NEITHER IS SOFTENED — both abort, on exactly the values the one assert aborted on before — and the split is
   what makes the first ACTIONABLE, because the remedies are different work: the second is a declaration to
   fix, and the first is a FORK the operation does not yet ask for. A refusal is an END STATE for the second
   and an INTERMEDIATE one for the first: while it stands, the engine is declining to explore a world the
   unknown admits, which is the opposite of what this file's boundary rule is for.
   IT IS A MACRO SO THE ADDRESS IS THE CALLER'S. The emitter stamps __FILE__/__LINE__ where the assert
   EXPANDS, so the refusal names the body that read the member rather than this header — the same reason
   idl_dict_bool is a macro over idl_dict_bool_at, and the reason this is not a function taking a `bool`.
   `cond` and `v` are DCHECK operands and must be side-effect-free; `v` is read twice.
   `declared` is the member's IDL AS ITS OWN SPEC WRITES IT (`` `long` with a `= 0` default ``), because the
   declaration is what the next reader has to check it against, and `name` is the member's identifier because
   one name is declared by many dictionaries and only the pair with the site says which one refused. */
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
   was recorded and that it takes N steps to consume.
   A STEP RETURNS A STEP CODE AND IS HANDED THE DRIVING MACHINE'S HEADER, because a per-node effect ASKS
   QUESTIONS: HTML §6.6.7's insertion steps run §6.6.6's allow focus steps, whose second clause is §6.4.1's
   TRANSIENT ACTIVATION — unknown external state, so the answer is a FORK and not a `bool`. A walk that could
   only say "more remains" had nowhere to put that, which is why it stood at a DFAIL naming this signature.
   JS_STEP_YIELD = more remains, JS_STEP_FORK = the caller returns it and the walk is RE-ENTERED at the same
   node (its own phase is what stops the effects it already performed from running twice), 0 = the walk is
   done and the buffer has been released.
   AND IT MAY RUN THE PAGE'S CODE, WHICH IS A PROPERTY OF DOM §4.2.3 "Mutation algorithms"'s TWO PHASES AND NOT
   OF THIS TRANSPORT. The two are opposite and the standard states both in its own words. §4.2.3 defines the
   INSERTION STEPS with "These steps must not modify the node tree that insertedNode participates in, create
   browsing contexts, fire events, or otherwise execute JavaScript", so a request that parked on the page's
   code between two of them would be a timeline the standard forbids. Its POST-CONNECTION steps are the opposite by construction —
   insert step 10 collects staticNodeList up front precisely "because the post-connection steps can modify the
   tree's structure, making live traversal unsafe" — and HTML §4.12.1.1 "Processing model"'s own worked example
   REQUIRES the page's code to run between two of staticNodeList's entries: `body.append(script1, script2)`
   where script1's body removes script2 prints nothing, which is only decidable if script1 RAN before step 12
   re-read script2's connectedness. So a step may make a REQUEST (JS_STEP_CALL and its kin), and WHICH PHASE
   may make one is the DOM layer's invariant, asserted at the walk that knows the phase — never here, which
   knows only that something was recorded.
   `in` IS THE COMPLETION OF THE REQUEST THE WALK LAST MADE, owned by the walk exactly as a step machine owns
   its `cb_result`, and JS_UNDEFINED on every ordinary re-entry. A walk that has made no request and is handed
   something else is being delivered an answer to a question it never asked, which it asserts on. */
typedef struct {
    void *(*take)(JSContext *ctx);                 /* everything recorded so far, or NULL; leaves none behind */
    /* ONE node; a step code. `in`/`out_cb`/`out_argc` are the step-machine request contract, unchanged. */
    int   (*step)(JSContext *ctx, void *buf, JSStepHdr *h, JSValue in, JSValue **out_cb, int *out_argc);
    void  (*release)(JSContext *ctx, void *buf);
    bool  (*recorded)(void);                       /* is anything waiting to be taken */
    /* THE BUFFER ACROSS A FORK. A fork inside the walk snapshots the driving machine, which byte-copies its
       state and re-takes only what a `visit` names — so a buffer left unvisited would be ONE allocation two
       flows both walk and both free, and the two arms would share a cursor. It is visited exactly as a step
       machine visits its own owned storage, and the DOM layer performs it because only that layer knows how
       the buffer is laid out. */
    void  (*visit)(JSContext *ctx, void **buf, JSStepVisit *v);
    /* THE HALF THE `visit` CANNOT CARRY, for a walk ABANDONED mid-request. The visit is the one list of what
       the buffer OWNS, and the driver's teardown discharges it — but a walk that parked on the page's code can
       also be HOLDING something that is not a reference, and nothing discharges that. HTML §8.1.4.6 "Runtime
       script errors" step 6.1's ERROR REPORTING MODE is the one: a nested program's report takes it off the
       global and gives it back at its own end, and a flow dropped in between would leave the global in
       reporting mode for the rest of the session, silently swallowing every later report.
       It is the exact pair `custom_elements_queue_unlock` already is for the reaction queue, which is why it is
       called from the same place in the teardown and not folded into `release`: `release` runs on the walk's
       NORMAL 0 edge, and this runs on the abandoned one. Freeing a reference here would be the second list that
       teardown's fingerprint check exists to catch. */
    void  (*unlock)(JSContext *ctx, void *buf);
} IdlTreeSteps;
void idl_set_tree_steps(const IdlTreeSteps *ops);

/* THE DOCUMENT'S INSTALL IS DONE — no further member declaration can be correct. A component declares in its
   init and installs from the cached id, so a declaration reached from a wrapper or from a running flow is the
   per-object mint this asserts against. Called once by the entry, after the components are installed. */
void idl_args_seal(void);

/* WAS THIS MEMBER DECLARED BEFORE THE PLATFORM WAS SEALED? A component DECLARES in its init and INSTALLS from
   the cached id; an install carrying a FRESH id after the seal is a member being minted per wrapper or per
   REALM, which is the same bug twice. Asked at the install because that is where the member's NAME is. */
bool idl_declared_before_seal(int stepid);

/* RELEASE, IN TWO HALVES WITH TWO DIFFERENT LIFETIMES — see idl_args.c, where the split is argued.
   `idl_args_free` gives back what the pool INTERNED (the dictionary member atoms), so it needs a live runtime,
   and it asserts that no step machine is live: a flow parked inside an IDL member reads this pool at its
   teardown, so the FRONTIER must be released first.
   `idl_args_pool_free` gives back the pool's BLOCKS, and each one holds a JSTrampStepDef that JS_RegisterStepDef
   borrowed and requires to outlive the runtime — so it runs AFTER JS_FreeRuntime, beside idl_async_iter_free. */
void idl_args_free(JSContext *ctx);
void idl_args_pool_free(void);

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

/* WEB IDL §3.3.10 [PutForwards]'s SETTER FOR A READONLY ATTRIBUTE — declared once, here, and shared by every
 * attribute in the platform that carries the extended attribute. `attr_id` is §3.7.6 Attributes' `id` (the
 * attribute being assigned to) and `forward_id` is §3.3.10's identifier argument (the attribute on the object
 * that one references, which receives the assignment). Both must outlive the declaration only as far as this
 * call: the two names are INTERNED here, because §3.7.6 step 4.5.8.1's Get and step 4.5.8.4's Set are each a
 * keyed request that holds its atom across a suspension.
 *
 * IT IS A MACHINE AND EVERY CARRIER MUST USE THIS ONE. The two operations §3.7.6 states are both the page's
 * code — 4.5.8.1's Get is an accessor or a Proxy trap, and 4.5.8.4's Set is the forwarded-to attribute's
 * SETTER, which for HTML §7.2.2 The Window object's `location` is HTML §7.2.4 The Location interface's `href`
 * and therefore a NAVIGATION that suspends inside the assignment. A per-component copy of the five steps built
 * out of JS_GetPropertyStr/JS_SetPropertyStr is a C activation hosting that, which is the drive-to-completion
 * this engine aborts on; two such copies existed and are gone. It is also the shape that gets the SPEC wrong
 * quietly: both of them wrote 4.5.8.4's Throw flag as `true`, which manufactures a TypeError exactly where the
 * standard's `false` does nothing.
 *
 * The assigned value is passed to the forwarded-to setter UNCONVERTED, which is §3.7.6's own order: step 4.5.8
 * returns before step 4.6's conversion, so the type that converts is the FORWARDED-TO attribute's. */
int  idl_setter_id_put_forwards(JSContext *ctx, const char *attr_id, const char *forward_id);

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
/* §3.7.1's INTERFACE OBJECT for an interface that declares NO constructor: a function object whose `prototype`
   is `proto` and whose call and construct both throw a TypeError. The one way to build one — a NULL C function
   pointer is not "no constructor", it is a crash where the spec says TypeError. */
/* WEB IDL §3.7.5 Constants' DESCRIPTOR, stated ONCE and named by every constant this engine installs.
   Web IDL §3.7.5 says where a constant goes — "Constants are exposed on interface objects, legacy callback
   interface objects, interface prototype objects, and on the single object that implements the interface when
   an interface is declared with the [Global] extended attribute" — and then states the descriptor with no
   condition on it anywhere, Web IDL §3.7.5 again: "Let desc be the PropertyDescriptor{[[Writable]]: false,
   [[Enumerable]]: true, [[Configurable]]: false, [[Value]]: value}."
   THE THREE BITS ARE DERIVED FROM THAT SENTENCE AND FROM NOTHING ELSE. quickjs spells a JSCFunctionListEntry's
   attributes as the bits that are PRESENT, so [[Enumerable]] true is JS_PROP_ENUMERABLE, and [[Writable]] false
   and [[Configurable]] false are JS_PROP_WRITABLE and JS_PROP_CONFIGURABLE being ABSENT. A constant is the one
   IDL member whose descriptor has no parameter in it: §3.7.6's attributes compute [[Configurable]] from
   [LegacyUnforgeable], and §3.7.5 computes nothing, so there is one answer and this is it.
   IT IS A NAMED DECLARATION RATHER THAN A NUMBER AT EACH ROW BECAUSE THE NUMBER WAS WRONG EVERYWHERE. Every
   constant in this engine was installed with a prop_flags of `0` — non-writable and non-configurable, which
   §3.7.5 does want, and NON-ENUMERABLE, which it does not — so `Node.ELEMENT_NODE` and every one of its
   siblings was invisible to `for...in`, to `Object.keys` and to `JSON.stringify` of the interface object, on
   the interface object and on the prototype alike. Two components had reached the right answer independently
   and spelled it out by hand, which is the drift this ends: one right answer written twice is two places for
   the next constant to be added wrongly, and it was added wrongly at every other site for the life of the tree.
   NO SITE SPELLS THESE BITS. A row names this, so the day §3.7.5's descriptor is re-read there is one line to
   re-read it at — and a constant added with a bare `0` is then visibly a row that did not ask.
   RESIDUAL — THIS STATES THE DESCRIPTOR AND NOT THE TARGETS.
   NOT COVERED: §3.7.5's first sentence obliges a constant onto the interface OBJECT as well as the interface
   prototype object, and that is two install calls a component makes by hand; nothing here can see that a
   component made only one. It is a residual and not a DFAIL because the flags are now right wherever a call
   was made — the code is correct for what it does and narrower than §3.7.5.
   WHAT THE NEXT DIFF BUILDS: a dev-only check that walks a constants table against a target and asserts each
   name is an own data property whose three attributes are exactly this — called at each install site, so a
   missing second target and a hand-rolled descriptor both fire at the origin instead of being read off a page.
   HOW ITS ABSENCE WOULD SHOW: `Node.ELEMENT_NODE` answering 1 while `Node.prototype.ELEMENT_NODE` is undefined
   (or the reverse), which a page reads and no instrument in this tree currently asks about — engine/idlgen.mjs
   audits which members EXIST and nothing about the attributes they are installed with. */
#define IDL_CONSTANT_PROP_FLAGS  JS_PROP_ENUMERABLE

/* WEB IDL §3.8 Platform objects implementing interfaces' DESCRIPTOR FOR THE PROPERTY AN INTERFACE PUTS ON A
   GLOBAL, stated ONCE and named by every site that puts one there.
   WHERE THE DESCRIPTOR ACTUALLY IS, because it is not where a reader looks first. Web IDL §3.7 Interfaces says
   only WHICH property exists — "The name of the property is the identifier of the interface, and its value is
   an object called the interface object" — and states no attributes at all; an edition that did state them
   inline is what `idlharness.js` still quotes in its own comment, and quoting a retired edition is not a
   citation. The current text states them in §3.8's `define the global property references`, which for an
   interface says "Perform DefineMethodProperty(target, id, interfaceObject, false)." — and ECMAScript §10.2.8
   DefineMethodProperty ( homeObj, name, closure, enumerable ) is where the descriptor is written down: "Let
   propertyDesc be the PropertyDescriptor { [[Value]]: closure, [[Writable]]: true, [[Enumerable]]: enumerable,
   [[Configurable]]: true }." With §3.8's `false` substituted for `enumerable` that is {writable, configurable},
   and NOT enumerable — which is the whole of the defect this ends.
   THE THREE BITS ARE DERIVED FROM THOSE TWO SENTENCES AND FROM NOTHING ELSE. quickjs spells a property's
   attributes as the bits that are PRESENT, so [[Writable]] true is JS_PROP_WRITABLE, [[Configurable]] true is
   JS_PROP_CONFIGURABLE, and [[Enumerable]] false is JS_PROP_ENUMERABLE being ABSENT.
   §3.8 REACHES FOUR KINDS OF OBJECT WITH THE IDENTICAL CALL and they are therefore ONE band, which is not
   obvious from the local variable a site happens to hold: an interface object, a [LegacyWindowAlias] of one, a
   legacy factory function ("Perform DefineMethodProperty(target, id, legacyFactoryFunction, false)"), a §3.11.1
   legacy callback interface object, and a §3.13.1 namespace object. `Image`, `NodeFilter` and `NamedNodeMap`
   are three different Web IDL constructs answering to one descriptor.
   IT IS A NAMED DECLARATION RATHER THAN A NUMBER AT EACH SITE BECAUSE THERE WAS NO NUMBER AT ALL. Every one of
   these properties was installed with JS_SetPropertyStr, which is an ordinary [[Set]] — §10.1.9.2 OrdinarySetWithOwnDescriptor
   creates the missing property through CreateDataProperty, whose descriptor is writable AND ENUMERABLE AND
   configurable. Two of the three bits were what §3.8 asks for and the middle one was not, so `URL`, `Node`,
   `Event` and every other interface name showed up in `for (var k in globalThis)`, in `Object.keys(globalThis)`
   and in a `JSON.stringify` of the global, in a way no browser does.
   NO SITE SPELLS THESE BITS: a site names idl_define_global_property_reference, and an interface installed with
   a bare JS_SetPropertyStr is then visibly a site that did not ask. */
#define IDL_INTERFACE_OBJECT_PROP_FLAGS  (JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)

/* THE INSTALL SITE, CAPTURED AT THE CALLER — the address every member-placing entry below carries, and the
 * reason each of them is a MACRO over an `_at` function rather than a function.
 *
 * WHY IT EXISTS. Web IDL §3.7.6's and §3.7.7's continue-step is asked ONCE, inside idl_args.c, by every entry
 * here; the DCHECK it now carries — a member being installed on the realm's global that no §3.3.8 [Global]
 * interface declares — is therefore written at ONE line, and a DCHECK stamps the file and line it is WRITTEN
 * at. With ~990 call sites reaching that one line, the crash named a remedy with no object: "route the
 * install", with nowhere to route. CLAUDE.md's rule for exactly this shape is that the site TRAVELS WITH THE
 * OPERATION, captured at the caller and threaded to the check.
 *
 * WHY A MACRO AND NOT A HELPER. A wrapper introduced to share the capture re-creates the defect it was
 * reaching for: __FILE__ and __LINE__ inside a function are that FUNCTION's, so one more forwarding hop would
 * name idl_args.c again for every caller. A function-like macro is expanded AT THE CALL, so the pair is the
 * caller's by construction and no call site had to be edited to get it — the 988 existing calls are unchanged
 * text and now carry their own address.
 *
 * WHY THE PAIR IS REQUIRED AND NOT DEFAULTED. Every `_at` entry takes both, so a caller that reaches one
 * without a site DOES NOT COMPILE. A defaulted or optional site is what lets an unconverted caller masquerade
 * as one with nothing to say, and the whole worth of this address is that it cannot be absent.
 *
 * A FORWARDER THAT CANNOT NAME ITS CALLER SAYS SO BY NAME. `IDL_SITE_INTERNAL` is what an idl_args.c-internal
 * path types INSTEAD of IDL_SITE, and the difference is not cosmetic: IDL_SITE inside idl_args.c would stamp
 * idl_args.c and read as an install site, which is a LIE in exactly the form this whole mechanism exists to
 * remove. It has NO USER TODAY — every forwarder in the chain (the two [Exposed] gates here, the shared
 * accessor definer, and core/events/event_target.h's handler installer) carries its own caller's pair — and
 * it is declared anyway because the alternative a future forwarder reaches for is `IDL_SITE`, silently. NULL
 * is not an option either way: the check DCHECKs the file pointer. */
#define IDL_SITE           __FILE__, __LINE__
#define IDL_SITE_INTERNAL  "core/idl_args.c (an internal path with no caller site to carry)", -1

/* §3.7.6 computes ONE field from §3.4.10's [LegacyUnforgeable]: "Let configurable be false if attr is
   unforgeable and true otherwise". Nothing else about an attribute differs, so the extended attribute is this
   one argument rather than a second install function. */
typedef enum {
    IDL_ATTR_REGULAR,       /* §3.7.6: [[Configurable]] true */
    IDL_ATTR_UNFORGEABLE,   /* §3.4.10 [LegacyUnforgeable]: [[Configurable]] false */
} IdlAttrForge;

/* A READONLY ATTRIBUTE WHOSE VALUE THE REALM ALREADY HOLDS: `window`, `document`, `customElements`. (A
   MessageChannel's `port1`/`port2` stood in that list and are NOT this form — §9.4.2's two getters read the
   channel's own record off the receiver and brand-check it, which is what an attribute of an ordinary
   interface does; this form is for a member of the [Global] one, and the brand it applies says so.)
   §3.7.6 makes every attribute an ACCESSOR, and a value that never changes is still
   one — the alternative that reads plausible (a data property, since the getter would compute the same answer
   forever) answers getOwnPropertyDescriptor wrongly and, from JS_SetPropertyStr, is WRITABLE, so a page can
   replace a member the spec does not let it touch. Not idl_install_replaceable_value: that installs §3.7.6's
   [Replaceable] setter and these members are readonly. Takes ownership of `value`. */
void idl_install_value_attribute_at(JSContext *ctx, JSValueConst target, const char *name, JSValue value,
                                    IdlAttrForge forge, const char *at_file, int at_line);
#define idl_install_value_attribute(ctx, target, name, value, forge) \
    idl_install_value_attribute_at((ctx), (target), (name), (value), (forge), IDL_SITE)

/* Agent teardown for the §3.4.2 declaration table — the shape idl_args' other per-agent
   tables already use. */
void idl_lenient_setters_free(void);

/* And for §3.7.3's [Replaceable] `target` table, which is the same shape for the same reason. */
void idl_replaceable_targets_free(void);

/* And for §3.7.6's RECEIVER-STATING ATTRIBUTE table, which is the same shape for the same reason. */
void idl_this_getters_free(void);

JSValue idl_interface_object(JSContext *ctx, const char *name, JSValueConst proto);

/* WEB IDL §3.8's `define the global property references`, as the ONE door an interface's name reaches the
   global through — see IDL_INTERFACE_OBJECT_PROP_FLAGS above for the descriptor and where it is stated.
   IT TAKES AN OBJECT RATHER THAN MINTING ONE, which is why it is not folded into
   idl_install_interface_object_exposed: that entry mints over idl_illegal_ctor, so it is §3.7.1's object for an
   interface that declares NO constructor, and an interface that DOES declare one would have `new X()` replaced
   by a TypeError if it were routed there. §3.8 does not care which of its four kinds the object is — it names
   `interfaceObject`, `legacyFactoryFunction` and a namespace object in three steps whose only difference is
   which mint produced the argument — so the mint stays at the component that knows its interface and the
   DEFINE is here.
   TAKES OWNERSHIP of `object`, exactly as the JS_SetPropertyStr every site used to call did, so a conversion is
   the call and nothing else. `global` is BORROWED. */
void idl_define_global_property_reference(JSContext *ctx, JSValueConst global, const char *id, JSValue object);
/* WEB IDL §3.4.11 [LegacyWindowAlias] — §3.8's step 3.1.4, WHOLE, for one of the extended attribute's
 * identifiers. Web IDL §3.4.11 [LegacyWindowAlias]: "If the [LegacyWindowAlias] extended attribute appears on
 * an interface, it indicates that the Window interface will have a property for each identifier mentioned in
 * the extended attribute, whose value is the interface object for the interface." Web IDL §3.7 Interfaces says where and what: "If the
 * [LegacyWindowAlias] extended attribute was specified on an exposed interface, then for each identifier in
 * [LegacyWindowAlias]'s identifiers there exists a corresponding property on the Window global object. The name
 * of the property is the given identifier, and its value is a reference to the interface object for the
 * interface".
 *
 * THE VALUE IS THE INTERFACE OBJECT ITSELF AND NOT A SECOND ONE. §3.8 step 3.1.4.1.1 performs
 * "DefineMethodProperty(target, id, interfaceObject, false)" over the SAME `interfaceObject` step 3.1.2 built
 * and step 3.1.3 already defined under the interface's own identifier — so `globalThis.webkitURL ===
 * globalThis.URL` is true, `webkitURL.name` is "URL", and a page's `x instanceof webkitURL` is the same brand
 * check as `x instanceof URL`. A caller therefore passes a JS_DupValue of the object it is about to define (or
 * has defined) as the interface object, never a fresh mint: two objects would answer `===` wrong and give the
 * alias a `prototype` of its own. Ownership follows the door's — the reference is CONSUMED on every path.
 *
 * ONE CALL PER IDENTIFIER, because §3.8 step 3.1.4.1 is a loop over the identifiers and 3.1.4.1.1 is one
 * define. A variadic taking the list would be the NULL-terminated argument scan this project has already been
 * miscompiled into an infinite loop by, for a saving of one line at the one corpus interface that has two.
 *
 * WHY IT IS A SEPARATE ENTRY AND NOT AN ARGUMENT TO THE DOOR: step 3.1.4 carries a condition the other four
 * DefineMethodProperty steps do not — "and target implements the Window interface" — and that condition is a
 * CONSTANT OF THE ALGORITHM rather than a property of the construct. It is not a fallback selecting against
 * anything (§C-stack's test: delete every alias in the corpus and the question still has to be asked of the
 * next one), and it is not a second door: this entry DEFINES nothing itself, it asks step 3.1.4's condition and
 * calls idl_define_global_property_reference, which stays the one place a name reaches a global. */
void idl_define_legacy_window_alias(JSContext *ctx, JSValueConst global, const char *id,
                                    JSValue interface_object);
/* §3.11.1's LEGACY CALLBACK INTERFACE OBJECT — what a callback interface on which constants are defined puts
   on the global. It is a BUILT-IN FUNCTION OBJECT ("Let F be CreateBuiltinFunction(steps, 0, id, « », realm)"
   over steps that throw a TypeError), which is why the spec's own note says `typeof` answers "function"; an
   ordinary object answers "object", and that is a fact a page reads. A callback interface has NO interface
   prototype object, so there is no §3.7.3 tag anywhere on it and this call is the only statement of which
   interface the constants installed on the returned object belong to. */
JSValue idl_callback_interface_object(JSContext *ctx, const char *name);

/* MINT a declared member's function object without installing it — for an internal door a C caller holds and
   calls, rather than a property a page reads. There is no `length` to pass here either: the object carries
   Web IDL §3.7.7 Operations' number, derived from the declaration (see idl_member_length_of). */
JSValue idl_step_function(JSContext *ctx, const char *name, int stepid);
/* The interface object for a declared CONSTRUCTOR — Web IDL §3.7.1 Interface object, whose `length` is the
   same sentence §3.7.7 states over the effective overload set for constructors, so it is derived here too and
   there is nothing for a caller to state. `new Event()` shipped with the declared arity 2 where §3.7.1
   computes 1, which is a number a page reads. */
JSValue idl_step_constructor(JSContext *ctx, const char *name, int stepid);

void idl_slowest_reset(void);
int64_t idl_slowest_step(const char **name);
/* The same window's TOTAL across every member step, and how many there were. The max alone cannot separate one
   very slow call from very many short ones. */
int64_t idl_step_total(long *count);

/* §3.7.3's @@toStringTag on an interface PROTOTYPE object: the interface's identifier, non-writable,
   non-enumerable, configurable. Every interface prototype has one, so every interface calls this.
   IT ALSO ASSERTS §3.7.3's PROTO STEP, against the generated browser/idl_inheritance.h — the [[Prototype]] of
   the object being tagged must be the interface prototype object of the interface the IDL says it inherits (or
   this realm's %Object.prototype% / %Error.prototype% on §3.7.3's two intrinsic arms). That is the one fact
   engine/idlgen.mjs's gap audit STANDS ON and cannot itself check: it credits a base's installed members to
   everything that inherits it, so a prototype built over the wrong parent reads COMPLETE for every member of
   the parent the IDL names while a page reaches none of them.
   IT IS ALSO WHERE §3.7.3's [Unscopable] BLOCK RUNS, which is the one thing about this call a reader would not
   guess from its name. §3.3.14 [Unscopable] defers its steps — "See § 3.7.3 Interface prototype object for the
   specific requirements that the use of [Unscopable] entails" — and §3.7.3 mints ONE OrdinaryObjectCreate(null)
   per interface prototype object, fills it with the identifiers of the interface's EXPOSED [Unscopable]
   members, and defines it under %Symbol.unscopables%. That is per INTERFACE and not per member, and it runs
   BEFORE the members are defined, so this call is the only point in this engine at which it can be asked:
   nothing standing at a member's install knows which interface it is on or whether it is the last one. The
   member list is browser/idl_unscopables.h, generated from the real .idl. */
void idl_interface_tag(JSContext *ctx, JSValueConst proto, const char *iface);
#if APICLIENT_DEV
/* THE OTHER END OF THAT BLOCK, which cannot be asked at the call above: §3.7.3 defines %Symbol.unscopables%
   BEFORE it defines the interface's members, so at the mint not one id is on the prototype yet. This walks the
   %Symbol.unscopables% object each of this realm's tagged prototypes actually carries and asserts every id in
   it names an own property of that prototype — "member's identifier" is what §3.7.3's loop writes, so an id
   whose member this engine has not built is a name in that object a page cannot reach.
   CALLED WHERE THE PER-REALM INTRINSIC LIST ENDS, which is a §3.7.6/§3.7.7 condition (every member installed)
   and NOT §3.8's (no further property reference), and the two have different populations — core/realm.h's
   owed-half entry is documented as not auditing a realm that reaches no document install, and every interface
   with a row here installs its members in the same function that tags its prototype. It reaches the objects
   through core/realm.h's §3.7.3 census, which is why that census records the prototype and not only the name. */
void idl_assert_unscopables_name_members(JSContext *ctx);
#endif

/* THE SAME CLASS STRING ON AN OBJECT THAT IS NOT AN INTERFACE PROTOTYPE OBJECT, so §3.7.3's proto step does not
   govern it and is not asserted. Exactly one object needs this: HTML §7.2.3 The WindowProxy exotic object's
   prototype, which carries WINDOW's class string ("There is no WindowProxy interface object") while the real
   §3.7.3 Window interface prototype object is a different object core/frame/window.c builds over §3.7.4's named
   properties object. Deliberately not idl_interface_tag, for the reason idl_namespace_tag and
   idl_async_iterator_tag are: which KIND of object is tagged is a fact the C states rather than one the auditor
   guesses — and engine/idl_installed.mjs seeds attribution from both, so the members installed on this object
   are still credited to the interface it names. */
void idl_class_string(JSContext *ctx, JSValueConst obj, const char *iface);

/* §3.13.1's CLASS STRING ON A NAMESPACE OBJECT: "The class string of a namespace object is the namespace's
   identifier" — so `Object.prototype.toString.call(console)` is "[object console]", with §3.2's same
   non-writable, non-enumerable, configurable descriptor.
   IT IS DELIBERATELY NOT idl_interface_tag. A namespace object is not an interface prototype object: it holds
   the namespace's operations DIRECTLY (§3.13.1 steps 2-4) rather than being the prototype of anything, and the
   §3.7.3 tag is what engine/idl_installed.mjs reads to decide which INTERFACE a file's installs belong to.
   Tagging a namespace with the interface form would file twenty operations under an interface no IDL defines;
   this states which NAMESPACE they belong to, which is a different fact the auditor reads separately —
   exactly the reason idl_async_iterator_tag is its own statement too. */
void idl_namespace_tag(JSContext *ctx, JSValueConst ns, const char *identifier);

/* §3.7.10.2's class string on an ASYNCHRONOUS ITERATOR PROTOTYPE OBJECT: the interface's identifier
   concatenated with " AsyncIterator". It is deliberately NOT idl_interface_tag — that object is not an
   interface prototype object and the members installed on it (§3.7.10.2's `next` and `return`) are not the
   interface's, so the two statements must not be the same one. */
void idl_async_iterator_tag(JSContext *ctx, JSValueConst aproto, const char *iface);

/* §3.2.27's CREATE FROZEN ARRAY, over an Array the caller has already filled: SetIntegrityLevel(array, frozen).
   AN ARRAY IS NOT FROZEN BY PREVENTING EXTENSIONS — it always carries an own `length` and `length` is writable,
   so `Object.isFrozen` answers false afterwards and a page can still truncate the array in place. Every own
   property has to lose writable and configurable, `length` included.
   ONE implementation, because FrozenArray is one TYPE and not a thing each member re-derives: it was written
   out inside MessageEvent's `ports` conversion, and the second member that needed one — NavigatorLanguage's
   `languages` — got only the preventExtensions half and shipped an array the spec calls frozen and a page could
   rewrite. Returns <0 with an exception pending. */
int idl_freeze_array(JSContext *ctx, JSValueConst arr);

/* WEB IDL §3.3.7 [Exposed]'s CONDITIONAL EXPOSURE ATTRIBUTES — the extended attributes that decide whether a
 * member EXISTS in a realm, as opposed to what it answers. The "is exposed in realm" algorithm defined under
 * that heading is four steps and this enum is the ones that are not about which global the member is on; the
 * one that IS, step 1, is `idl_exposed_in_realm` below. (It read §3.9 here
 * and in idl_args.c, which is "Legacy platform objects" — see the note at idl_exposed for why a number that
 * RESOLVES to the wrong real section is the one shape engine/citegen.mjs cannot see without a title beside it.)
 *
 * §3.3.13's [SecureContext] REMOVES THE MEMBER. The spec's own example is unambiguous — "in a non-secure
 * context there will be no `calculateSecretResult` property on ExampleFeature.prototype" — so this is never a
 * getter that throws and never one that answers undefined. A page distinguishes all three: `'deviceMemory' in
 * navigator`, `if (navigator.deviceMemory)` and a try/catch around the read go three different ways, and each
 * of those is a branch this engine exists to explore correctly.
 *
 * THE ATTRIBUTE IS DATA THE COMPONENT STATES, NOT A CONDITION IT EVALUATES. A `if (secure) install(...)` at
 * each gated member is the hand-picked list in miniature: every member added afterwards is exposed everywhere
 * by default and nothing says so, and each site re-derives what [SecureContext] MEANS (absent? throwing?
 * undefined?) with nothing to keep the derivations equal. So the member's install carries its IDL's exposure
 * the same way it already carries its IDL's argument types, and this file — the one place every declared
 * member converges on — is where the condition is asked. A component that states the attribute has done
 * everything the IDL asks of it.
 *
 * STEP 1 IS NOT IN THIS ENUM AND IS NOT ABSENT EITHER — it is `idl_exposed_in_realm` below, and the reason it
 * is a SEPARATE question rather than another value here is that §3.3.7's conditions are ORTHOGONAL: an
 * interface is routinely `[Exposed=(Window,Worker), SecureContext]`, so a single scalar could not state both
 * and one of the two would have to be dropped at every such member. This enum is the CONDITIONAL-ATTRIBUTE
 * axis (steps 2 and 3); the EXPOSURE-SET axis (step 1) is decided by an identifier and a realm.
 *
 * AND THAT LAST CLAUSE USED TO END `and neither of those is a thing a component has to state`, WHICH IS TRUE
 * OF A §3.8 IDENTIFIER AND FALSE OF A MEMBER — the difference being the whole reason the member half of step 1
 * has no carrier anywhere in this engine. §3.3.7 [Exposed] is declared to apply to an individual interface
 * member, interface mixin member, or namespace member, and its `is exposed in realm` algorithm is written over
 * a construct that may be a member; §3.7.6 Attributes then asks it per attribute, in the one step of `define
 * the attributes` that removes one — "If attr is not exposed in realm, then continue." So a MEMBER owes step 1
 * exactly as an interface object does. What it does NOT have is a name that answers it: §3.3.7's own note says
 * "the exposure set of its members is a function of the interface that includes them", so `performance` is
 * Window-and-Worker through `WindowOrWorkerGlobalScope` while `innerWidth` is Window-only through CSSOM VIEW's
 * `partial interface Window`, and the two are indistinguishable from the identifier alone. `idl_exposed_in_realm`
 * below therefore CANNOT be the member's answer — browser/idl_exposure.h is keyed by the identifier §3.8 puts on
 * a global, a member has no row there, and a name with no row is exposed — so asking it of a member name is a
 * check whose two sides cannot disagree.
 *
 * THE MEMBER-SIDE EXPOSURE SET IS `idl_member_exposed_in_realm` BELOW, AND IT IS DERIVED RATHER THAN STATED.
 * The paragraph above is exactly right that a member's set cannot be read off its identifier BY REASONING; it
 * does not follow that a component has to state it, and this sentence used to say it did: it asserted that the
 * member-side exposure set was data the component states, on the ground that only the component declaring a
 * member knows it, with the next diff named as one more field on every install. That clause is RETIRED and the
 * correction is recorded here rather than deleted, because a reader who re-derives the abandoned design will
 * build it: the fact is a CORPUS fact, in the member's own extended attributes, so a C declaration restating it
 * would be the hand-kept second copy §Browser half bans — and the engine already had the machinery, since
 * engine/idlgen.mjs computes §3.3.7's `get the exposure set of a construct C` for its own gap audit and
 * browser/idl_exposure.h is already a table keyed by a name one vocabulary over. What is genuinely the
 * component's to state is [SecureContext], which no artifact can decide for it; that is why THIS enum stays.
 * The measured cost of the retired clause would have been every install site's signature for a fact no site
 * knows better than the corpus does.
 *
 * WHAT THIS PARAGRAPH USED TO ARGUE, because it was true and it was also the blocker: that [Exposed] is
 * decided by which global a component installs on, that this engine has exactly one global kind — no
 * WorkerGlobalScope — and that every member's exposure set was therefore trivially satisfied. Trivially
 * satisfied is what an unasked question looks like from inside the only realm that ever asked it. With step 1 unasked there was no
 * way to BUILD a realm that gets the `[Exposed=Worker]` surface and not Window's, so the only way to run a
 * worker script was in a Window realm, where `document` exists — a fidelity bug, not a slice. The engine still
 * has no WorkerGlobalScope; what it has now is the axis one has to be built on.
 *
 * [CrossOriginIsolated] is decided
 * by HTML §7.2.2's cross-origin isolated capability, which core/frame/agent_cluster.h now ANSWERS — false for
 * every environment this build makes, because §7.1.3.2's browsing context group switch is what would set the
 * group's isolation mode to `concrete` and nothing performs it yet (the COOP and COEP headers themselves DO
 * reach the engine, and a response that would need the switch crashes by name). So the
 * condition is absent from this enum because no member in this build carries the attribute, not because the
 * capability cannot be asked: the day one does, it is a value here calling that component, and the gate below
 * grows a case rather than a second gate somewhere else. */
typedef enum {
    IDL_EXPOSED = 0,        /* the member's IDL carries no exposure condition — it is in every realm */
    IDL_SECURE_CONTEXT,     /* [SecureContext] — ABSENT, not throwing, in a non-secure realm */
} IdlExposure;

/* WEB IDL §3.3.7 [Exposed]'s "is exposed in realm", ASKED — the one statement of that algorithm's conditions,
 * and the reason it is declared here is narrow enough to state as a rule: A CALLER THAT PUTS SOMETHING ON A
 * REALM MAY NOT ASK IT. An install states its IDL's exposure as DATA (the `_exposed` installers' parameter) and
 * the gate is asked once, inside this file, for every member alike — an `if (idl_exposed(...))` at an install
 * site is the per-member conditional that parameter exists to remove, and every such site re-derives what
 * [SecureContext] MEANS with nothing keeping the derivations equal.
 * WHAT MAY ASK IT IS A CALLER THAT INSTALLS NOTHING: core/platform.c's witness list, which is an ORACLE over
 * the finished realm rather than a builder of one. It states independently which names a realm's global must
 * and must not carry and then disagrees with reality, so it has to decide the same condition — and a witness
 * that spelled the condition itself would be a second statement of §3.3.7 step 2, which is the restated rule an
 * auditor must never contain. See idl_args.c for the full argument, including why the oracle states each name's
 * exposure itself instead of reading back what the gate did. */
bool idl_exposed(JSContext *ctx, IdlExposure exposure);

/* WEB IDL §3.3.7 [Exposed]'s STEP 1, ASKED OF ONE IDENTIFIER — "If construct's exposure set is not `*`, and
 * realm.[[GlobalObject]] does not implement an interface that is in construct's exposure set, then return
 * false".
 *
 * IT TAKES AN IDENTIFIER AND NOT AN ANNOTATION, WHICH IS THE WHOLE DIFFERENCE FROM `idl_exposed` ABOVE. A
 * conditional attribute is a fact about a MEMBER that only the component knows it carries, so the component
 * states it as data. An exposure SET is a fact about a NAMED CONSTRUCT that the corpus already states, and
 * §3.8 `define the global property references` is handed that name — so a C table repeating it per install
 * site would be the third copy of a fact whose first copy is the `.idl` this project already reads. Both sides
 * of the intersection are therefore GENERATED: browser/idl_exposure.h holds §3.3.7's exposure set per
 * identifier and §3.3.8 [Global]'s global names per global interface, both emitted by engine/idlgen.mjs from
 * the same derivation its own NOT-EXPOSED category is computed with, so the audit and the engine cannot
 * disagree about what §3.3.7 says.
 *
 * A NAME WITH NO ROW IS EXPOSED. Absence of evidence must not remove a property from a realm — an identifier
 * the corpus does not declare keeps what it has today — so the rows that carry information are the ones that
 * can EXCLUDE, and a table that lost a row makes the engine no stricter than it was.
 *
 * WHERE IT IS ASKED IS §3.8's ONE ENTRY, `idl_define_global_property_reference`, which every interface object,
 * legacy factory function and namespace object in this engine already converges on. That is the same rule the
 * `_exposed` installers state from the other side — the question is asked at the one place, never at eighty
 * call sites — with the difference that here no caller states anything at all.
 *
 * AND THAT IS THE WHOLE OF THE AXIS THIS ENTRY CAN CARRY, WHICH IS NARROWER THAN §3.3.7 STEP 1. §3.8 places
 * an IDENTIFIER on a global; §3.7.6 Attributes places a MEMBER on it, by the other arm of its opening prose —
 * "Regular attributes are exposed on the interface prototype object" unless the interface is [Global] — and it
 * asks step 1 per attribute at "If attr is not exposed in realm, then continue." This entry cannot answer THAT
 * ask: a member has no row, a name with no row is exposed, so it would return true for every member in every
 * realm. That ask is `idl_member_exposed_in_realm` below, over a table keyed by the other vocabulary. */
bool idl_exposed_in_realm(JSContext *ctx, const char *identifier);

/* WEB IDL §3.3.7 [Exposed] STEP 1 ASKED OF A MEMBER — the ask §3.7.6 Attributes makes at "If attr is not
 * exposed in realm, then continue." and §3.7.7 Operations makes at "If op is not exposed in realm, then
 * continue.", for a member the [Global] arm of each puts on the realm's global object rather than on a
 * prototype.
 *
 * IT IS A SECOND TABLE AND NOT A SECOND LOOKUP, for the reason the entry above ends on: §3.8's identifiers and
 * §3.7.6's members are two vocabularies, IDL_EXPOSURE is keyed by the first, and a member has no row in it. The
 * rows come out of the same corpus by the same algorithm — engine/idlgen.mjs runs §3.3.7's `get the exposure
 * set of a construct C` over every member of every [Global] interface — so the two tables cannot disagree about
 * what §3.3.7 says, which is the property that made generating the first one worth doing.
 *
 * THE ANSWER IS A SOUND OVER-APPROXIMATION, AND THAT IS A NAMED RESIDUAL RATHER THAN A GAP. WHAT IS NOT
 * COVERED: §3.7.6 asks its question of ONE attribute of ONE definition, and this entry is handed only a NAME —
 * so its row is the union over every interface the realm's global implements, its ancestors included, and it
 * refuses only a name that NO such interface declares as exposed here. A realm's global that implements two
 * interfaces declaring one identifier keeps the property when either is exposed, where §3.7.6 would have
 * removed one member and kept the other; since both arms end in a property under that name, no page can see
 * the difference. WHAT THE NEXT DIFF BUILDS: the declaring interface as data at the install — which is the
 * same missing argument `idl_check_global_target`'s own DFAIL already names, so it is one thing to build and
 * not two — after which the question is keyed by (interface, member) and the union goes. HOW ITS ABSENCE WOULD
 * SHOW: a member this engine installs under a name some OTHER interface of the same global also declares, with
 * only that other interface exposed here — measurable as a property present whose getter belongs to an
 * interface this realm does not implement, never as a missing name.
 *
 * A NAME WITH NO ROW IS EXPOSED, which is browser/idl_exposure.h's own rule and is the direction that cannot
 * remove something on absence of evidence. The generator omits a member whose exposure set is `*` and one
 * whose set is empty, because neither can EXCLUDE a realm and a set of no bits is how `*` is spelled.
 *
 * AND THAT SILENCE CARRIED TWO STATES, WHICH IS NOW ASKED APART RATHER THAN AVERAGED. The paragraph above used
 * to end "so a member installed on a global that the corpus does not declare on any [Global] interface keeps
 * its property, exactly as an unknown identifier keeps its own" — the RELEASE behaviour is unchanged and that
 * sentence is still true of it, and reading it as the whole answer is what made a real defect invisible: a
 * no-row name is EITHER a member whose §3.3.7 exposure set is `*` OR a name that is no [Global] interface's
 * member at all, and the second is a property on a global that no browser has. The IDL_GLOBALS row for THIS
 * REALM's [Global] interface tells them apart, and it is a strictly sharper discriminator than the chain-wide
 * union that used to stand here: that union asked whether a name is a member of ANY [Global] interface or of
 * anything one of them inherits, which `setTimeout` satisfies in a WORKER realm although §3.8 never writes it onto a
 * worker global. The row's `own` band is what §3.8 writes, so it refuses the chain-only names as well as the
 * names that are no member anywhere. The implementation DCHECKs it at the one call that knows the target is
 * the realm's global, and a release build takes the arm this paragraph always described.
 *
 * IT IS THE MEMBER NAME AND NOT AN ACCESSOR'S — the "get "/"set " prefix §3.7.6 puts on the FUNCTION OBJECT is
 * not part of the property key and not part of the corpus's identifier, so the string asked here is the one
 * the install was handed. */
bool idl_member_exposed_in_realm(JSContext *ctx, const char *member);

/* WEB IDL §3.3.8 [Global]'s GLOBAL NAMES of one global interface — "The [Global] extended attribute also
 * defines the global names for the interface" — which is the REALM side of §3.3.7 step 1's intersection. (The
 * requirement that an exposure set name only these is §3.3.7's own, "Each of the identifiers mentioned must be
 * a global name of some interface and be unique"; it is a real sentence and it belongs to [Exposed].)
 *
 * IT IS RESOLVED ONCE PER REALM, BY core/realm.c, from the interface name the host stated. A realm that named
 * an interface the corpus does not declare [Global] ABORTS here rather than at the first member that would
 * have been wrong about it, because a realm whose global names are zero is a realm every non-`*` construct is
 * absent from — a whole platform surface silently missing, which is the shape §3.3.7 step 1 can fail in. */
unsigned idl_global_names_of(const char *global_interface);

/* HTML §8.1.3.5 "Secure contexts" step 1.2's condition ("If global is a WorkerGlobalScope") and step 1.3's
 * ("If global is a WorkletGlobalScope"), read off the §3.3.8 [Global] global names above.
 *
 * A REALM STORES THE MASK, AND §8.1.3.5 ASKS ABOUT THE INTERFACE — so these are only sound because the two
 * agree over the corpus's own rows, which idl_args.c derives rather than assumes. Every caller is a step of
 * that algorithm; core/frame/secure_context.c is the one that runs it and core/realm.c is the one that decides
 * which fields a realm's environment is required to state. They take the MASK and not a JSContext because the
 * question is about §3.3.8's vocabulary and not about any realm — the realm is core/realm.h's to supply. */
bool idl_global_names_are_worker(unsigned global_names);
bool idl_global_names_are_worklet(unsigned global_names);

/* WEB IDL §3.8 Platform objects implementing interfaces' STEP 3.1.4's SECOND CONJUNCT — "and target implements
 * the Window interface" — read off the same §3.3.8 [Global] mask, and a THIRD question asked of it rather than
 * a third bit. It is a different algorithm from the two above (HTML §8.1.3.5's steps), which is why it has its
 * own name: the mask is a FACT about a realm, and every one of these is a QUESTION some step asks of it.
 *
 * IT IS DERIVED FROM THE TABLE AND NOT ASSERTED. Of browser/idl_exposure.h's nine IDL_GLOBALS rows, exactly one
 * carries IDL_GLOBAL_WINDOW, and it is `Window` — so a realm whose global names contain that bit and a realm
 * whose global object implements the Window interface are the same realms. The day webref declares a
 * second [Global] interface whose global names include `Window`, that stops being true, and idl_args.c's DCHECK
 * is what says so rather than an alias quietly appearing in a realm §3.8 excludes it from. */
bool idl_global_names_are_window(unsigned global_names);

/* WEB IDL §3.7.6 Attributes' NAME FOR AN ACCESSOR'S FUNCTION OBJECT — "Let name be the string \"get \"
 * prepended to attribute's identifier" for create an attribute getter, and "Let name be the string \"set \"
 * prepended to id" for create an attribute setter. The installers below perform it themselves and no caller of
 * one ever needs this; it is declared because a handful of members are defined at a RAW JS_DefinePropertyGetSet
 * instead, and every one of them was spelling the prefix by hand — half of them correctly. A prefix written at
 * N sites is a prefix that is wrong at some of them, which is the defect this composer was extracted to end, so
 * there is ONE place in the engine that writes it and the raw sites reach it here.
 *
 * `buf` is the caller's, at least IDL_ACCESSOR_NAME_MAX bytes, and the composed string is for the MINT ALONE —
 * never for a property key, a pool entry, or data a getter carries to name its member in a TypeError. See
 * idl_args.c for why those four readers must keep the bare identifier.
 *
 * ITS NEXT-DIFF CLAUSE WAS WRONG, AND THAT IS RECORDED HERE BECAUSE A CRASH-OR-RESIDUAL CLAUSE IS READ ONCE,
 * BY SOMEBODY WHO HAS ALREADY DECIDED TO DO THE WORK. RETIRED TEXT, unquoted because it is this header's own
 * and not a standard's — it said the raw sites are there because no installer form accepts a PLAIN C SETTER,
 * that idl_install_accessor takes a setter STEP id while js_handler_set is an ordinary C function, and that
 * the next diff builds an installer form taking a plain-C setter beside the IdlGetter. Its SPEC half was exact
 * and its remedy named a mechanism that must not be built.
 *   A PLAIN C SETTER ALREADY REACHES EVERY INSTALLER, one level up from where the clause was looking:
 * `idl_setter_id` takes an `IdlSetter` — a plain C body — and RETURNS a setter step id, which is precisely
 * what every installer's `setter_stepid` wants. core/dom/aria_mixin.c had been installing a step getter beside
 * an `idl_setter_id` plain-C setter through `idl_install_accessor_step` the whole time the clause stood. So
 * the named installer was not missing, it was redundant, and building it would have been the "second way of
 * doing this" that `idl_install_accessor_step`'s own declaration forbids further down in this header — a
 * second install shape for a case the pool already answers, which is exactly the drift this composer was
 * extracted to end.
 *   THE TELL WAS AVAILABLE WITHOUT LEAVING THIS FILE: the clause reasons about what an INSTALLER accepts, and
 * the question is what a DECLARATION accepts. "Takes a setter STEP id" is true and is not an obstacle, because
 * a step id is what a declaration hands you and never something a body has to already be.
 *
 * NAMED RESIDUAL — THE RAW SITES THEMSELVES. WHAT IS NOT COVERED: HTMLTemplateElement's `content`, and
 * AbortSignal's `aborted` and AbortController's `signal`, are defined by JS_DefinePropertyGetSet rather than by
 * an installer, so they get §3.7.6's descriptor and its name from their own call site and nothing checks that
 * they agree with the installers.
 *   ALL THREE ARE READONLY, so the plain-C-SETTER premise this residual was originally written on does not
 * describe a single one of the surviving sites — they have no setter to install. What actually separates them
 * is the GETTER'S C SHAPE against `IdlGetter`, which is `(ctx, this_val, magic)`. `js_template_content` and
 * `js_ctrl_get_signal` ALREADY HAVE EXACTLY THAT SHAPE and are minted JS_CFUNC_getter_magic, so those two can
 * install through `idl_install_accessor(..., getter, 0, -1)` — the readonly form, since a negative setter id
 * mints no setter — with no new mechanism whatever. `js_sig_get_aborted` is JS_CFUNC_generic
 * `(ctx, this_val, argc, argv)` and needs that one-line shape change first.
 *   WHAT THE NEXT DIFF BUILDS: nothing in this file. It converts `content` and `signal` to
 * `idl_install_accessor` as they stand, then changes `aborted` to the `IdlGetter` shape and does the same,
 * after which this declaration has no callers left and goes.
 *   `reason` WAS IN THAT LIST AND LEFT IT THROUGH A DESTINATION THIS CLAUSE DID NOT HAVE, which is recorded
 * rather than quietly dropped because the reasoning that put it here was sound and will be re-derived. The
 * clause sorted the four sites by whether their getter already had the `IdlGetter` SHAPE, and that is the right
 * question for a plain-C getter and the wrong one for `reason`: core/dom/abort.c's `reason` getter asks whether
 * the signal is aborted, so it can FORK, and a plain-C body has nowhere for the sibling to resume — it is a
 * MACHINE for a reason that has nothing to do with this header's descriptor argument, and it installs through
 * `idl_install_accessor_step` (whose own declaration below calls the plain-C form "what remains to be
 * converted"). So a raw site may leave this list by acquiring `IdlGetter`'s shape OR by becoming a machine, and
 * the second is not a smaller version of the first.
 *   HOW ITS ABSENCE SHOWS: a member added at a raw site keeps
 * §3.7.6's [[Enumerable]]/[[Configurable]] pair and its name under whoever wrote that line, so it can
 * differ from every installed member without any gate saying so — which is how `content` came to answer
 * `Object.getOwnPropertyDescriptor(HTMLTemplateElement.prototype,"content").get.name` with "content".
 *   AND THE ENUMERATION ABOVE HAS ALREADY BEEN WRONG ONCE, IN THE DIRECTION THAT MAKES THE WORK LOOK SMALLER.
 * It named two sites and there were five: core/dom/abort.c's three were missing from it for as long as it
 * stood, and they are the ones that hand-spell "get aborted"/"get reason"/"get signal" as string literals
 * instead of reaching this composer — so the very defect the clause describes was being committed by sites
 * the clause did not list. Re-derive the list before working from it; it is
 * `grep -rnE 'JS_DefinePropertyGetSet[[:space:]]*\(' engine/host --include=*.c` minus idl_args.c's own, and
 * that command is the durable half of this paragraph. IT MATCHES THE CONSTRUCT AND NOT THE NAME, which is one
 * character of regex and is owed here specifically: the surviving sites are in files that ARGUE about this
 * composer in their own comments, so a count of the name scores how faithfully a component documented itself
 * and reports a site for every paragraph written about one.
 * AND THE DESCRIPTOR IS NO LONGER THE ONLY THING A RAW SITE DECIDES FOR ITSELF. The installers mint every
 * plain-C attribute getter at one point, and that mint is what gives an attribute installed on the realm's
 * [Global] object §3.7.6's opening steps — the receiver resolution, §3.5's "getter" security check and the
 * Window brand. HTML §8.1.8.1's event handlers ARE Window attributes and WERE defined at the raw site, so the
 * whole family was installed on the global without them: `Object.getOwnPropertyDescriptor(window, "onload")
 * .get.call(crossOriginWindowProxy)` answered out of the reading realm where `onload` is absent from HTML
 * §7.2.1.3.1 CrossOriginProperties and a browser throws "SecurityError". That family now installs through
 * `idl_install_accessor_step`, which states §3.5's kind at the mint; the three remaining raw sites still do
 * not, and none of them is on a [Global] object, which is why this is the weaker half of their absence. */
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
