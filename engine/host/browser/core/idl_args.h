/* THE WEB IDL ARGUMENT COERCION, as one machine every member shares — see idl_args.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#define ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_iter.h"
#include "solver/concolic.h"   /* IDL_DCHECK_MEMBER asks whether a member CROSSED — see its comment below */

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
    /* `ByteString`. ToString and then §3.2.11's RANGE: every code point must be 0x00..0xFF, and one above that
       is a TypeError. That range IS the type — `new Response("", {statusText: "\u0100"})` throws — and it is
       stated here so no body has to remember it. */
    IDL_BYTESTRING,
    /* `USVString`. ToString and then §3.2.12's SCALAR VALUE conversion: every unpaired surrogate becomes
       U+FFFD. That replacement is the whole of what makes the type different from a DOMString, and every
       member of the URL surface takes one. */
    IDL_USVSTRING,
    /* `DOMString?`. Web IDL converts null AND undefined to the IDL value null before ToString is ever reached,
       so the body receives JS_NULL and never the string "null". textContent is the member that makes this
       load-bearing: `el.textContent = null` is "replace all with null", which removes the children and adds NO
       Text node, and stringifying it wrote the four characters `null` into the page's DOM instead. */
    IDL_DOMSTRING_NULLABLE,
    /* `USVString?` — the same null-and-undefined-become-null rule with §3.2.12's scalar value conversion after
       it. XHR §3.5.1's `optional USVString? username = null` is the member that needs it, and it needs it
       load-bearingly: `open(m, u, false, null)` must leave the parsed URL's username alone, where a
       DOMString-nullable would drop the surrogate replacement and a plain USVString would set it to the four
       characters "null". */
    IDL_USVSTRING_NULLABLE,
    /* THE INTEGER TYPES, each stating its WIDTH and its SIGN — which is all §3.2's conversion needs, and the
       whole of what tells them apart. Every one of them is ToNumber (the page's valueOf may run) and then the
       one arithmetic in idl_args.c: sign(x)·floor(|x|) taken MODULO 2^width, folded into range if the type is
       signed, with a non-finite value becoming 0.
       IT IS A MODULO AND NOT A CLAMP, for all of them. `new Response("", {status: 65736})` is status 200 in
       every browser, and `list.item(2**32)` is item 0 — a saturating conversion answers 65736 and 4294967296,
       and the range check that follows then throws or misses where the spec wraps. */
    IDL_LONG,             /* `long` — 32, signed */
    IDL_UNSIGNED_LONG,    /* `unsigned long` — 32, unsigned */
    /* `[EnforceRange] unsigned long` — §3.3.6 [EnforceRange], whose ARM of §3.2.4.9 Abstract operations'
       ConvertToInt REPLACES the modulo with a REFUSAL: a non-finite value, or one whose IntegerPart is outside
       the type's range, is a TypeError instead of a wrap. THE NUMBER HERE WAS §3.2.4.10, WHICH DOES NOT EXIST —
       §3.2.4 Integer types ends at §3.2.4.9 — and it read as plausible for as long as nobody opened it, then
       propagated into ten more sites in one diff because they took it from here. The attribute and the steps are
       two sections and the pair is what makes the claim checkable: §3.3.6 says which types carry it, §3.2.4.9
       says what it does. It is a separate type and not a flag for the same reason [Clamp] (§3.3.3) is: the
       extended attribute IS the conversion.
       Indexed Database §4.9's `advance([EnforceRange] unsigned long count)` is the member that needs it, and
       the difference is the whole of what a page can observe there — `cursor.advance(-1)` is a TypeError, where
       the modulo makes it a request to advance 4294967295 records that would walk the store to its end. */
    IDL_UNSIGNED_LONG_ENFORCE,
    IDL_UNSIGNED_SHORT,   /* `unsigned short` — 16, unsigned */
    IDL_LONG_LONG,        /* `long long` — 64, signed */
    /* `unsigned long long` — 64, UNSIGNED, and it is a separate type because the sign is observable. File
       System §2.5's `seek(unsigned long long position)` and `truncate(unsigned long long size)` are the members
       that need it: `truncate(-1)` is 2**64-1 bytes in a browser (a QuotaExceededError), where the signed
       conversion answers -1 and hands the algorithm a negative size no step of it is written for. */
    IDL_UNSIGNED_LONG_LONG,
    /* `[EnforceRange] unsigned long long` — §3.3.6 [EnforceRange] over §3.2.4.8 unsigned long long, and it is
       a separate row from the one above for the same reason IDL_UNSIGNED_LONG_ENFORCE is separate from
       IDL_UNSIGNED_LONG: the extended attribute IS the conversion, replacing the modulo with a refusal.
       ITS UPPER BOUND IS NOT 2**64-1 AND THAT IS §3.2.4.9's OWN FIRST STEP, not a bound chosen here.
       ConvertToInt(V, bitLength, signedness) opens "If bitLength is 64, then:" and sets upperBound to 2**53-1
       (the standard writes that as a superscript) — and says why in a note directly beneath it: "this ensures
       long long types associated with [EnforceRange] or [Clamp] extended attributes are representable in
       JavaScript's Number type as unambiguous integers". So the attribute NARROWS the type's range rather than
       merely refusing what falls outside it, and a declaration reusing the plain type's 2**64-1 would accept
       values a browser rejects.
       Streams §4.5.1 Interface definition's `ReadableStreamBYOBReaderReadOptions` writes
       `[EnforceRange] unsigned long long min = 1`, and the difference a page sees is two-sided:
       `reader.read(v, {min: 2**60})` is a TypeError from the TYPE where the wider bound reaches §4.5's own
       RangeError, and `reader.read(v, {min: 1.5})` reads with a minimum of 1 because IntegerPart truncates
       before the bounds are tested — [EnforceRange] refuses a value out of RANGE and never a fractional one. */
    IDL_UNSIGNED_LONG_LONG_ENFORCE,
    /* `[Clamp] long long`. The extended attribute REPLACES the modulo with §3.2.4.9: clamp to the type's range
       and round to the NEAREST integer, choosing the even one at a half. Blob's `slice(start, end)` is the
       member that carries it, and `slice(1.5)` starting at byte 2 rather than byte 1 is the difference. */
    IDL_LONG_LONG_CLAMP,
    /* `unrestricted double` — §3.2.8. ToNumber and nothing else: NaN and the infinities are VALUES of this
       type, which is exactly why QueuingStrategy's `highWaterMark` is declared with it and why the stream's
       own RangeError for a NaN mark has to be the STREAM's check rather than the type's. */
    IDL_UNRESTRICTED_DOUBLE,
    /* `double` — §3.2.7, and the RESTRICTION is the whole difference from the type above: ToNumber, and then a
       NaN or an infinity is a TypeError rather than a value. HTML §4.8.11.6's `attribute double currentTime`
       is what needs it — `video.currentTime = NaN` throws in every browser, and a member declared
       unrestricted would hand the seek algorithm a position no step of it is written for. The check belongs to
       the TYPE and not to the four setters that share it, for the reason every other row here does. */
    IDL_DOUBLE,
    /* A CALLBACK FUNCTION type — §3.2.19. The conversion is a brand check and nothing more: a callable crosses
       as itself and anything else is a TypeError. Declared rather than checked in the body because an optional
       member that is absent must NOT be rejected, and every body that wrote that test by hand is a body that
       can get it wrong once. */
    IDL_CALLBACK,
    /* AN ENUMERATION — Web IDL §3.2.18 Enumeration types. ToString, and then the result must be one of the
       values the IDL lists or it is a TypeError: `new Blob([], {endings: "bogus"})` throws, and an unrecognised
       value is never silently the default. The values are declared beside the POSITION (idl_arg_enum) or beside
       the dictionary MEMBER (IdlDictMember::values), because they are part of the type and a member's IDL may
       write two.
       OVER UNKNOWN EXTERNAL INPUT IT IS A FORK, WHICH IS WHY IT IS NOT A CROSSING TYPE — the same answer
       §3.2.3 boolean gets one type up, for the same reason and by a different arithmetic. The type's DOMAIN IS
       FINITE and the declaration states it: Web IDL §3.2.18 Enumeration types is "If S is not one of E's
       enumeration values, then throw a TypeError" followed by "Return the enumeration value of type E that is
       equal to S", so every world this conversion can complete in is either one of the N strings the IDL wrote
       or that one refusal. Crossing an
       unknown here does not defer the choice, it MOVES it: the placed concolic reaches a body that wanted a
       string and either aborts on it or answers from the SOLVER's value class — which is the collapse the
       pass-through exists to prevent, arriving at a type whose every world a `char *` already carries.
       AND THE WORLDS ARE ONES THE ALGORITHMS BEHIND THIS BOUNDARY OBSERVE APART. Fetch's `credentials` is
       "omit" / "same-origin" / "include", and which of them a request carries is whether it carries the
       person's cookies; `redirect` is "follow" / "error" / "manual"; `cache` names six. Picking any of them
       for a value nothing is known about deletes the rest. See idl_concolic_rule, which is where the reason
       lives, and idl_enum_fork, which is the one statement of the ask both boundaries make. */
    IDL_ENUM,
    /* A NULLABLE ENUMERATION — `NavigationType? navigationType = null`, and the difference from IDL_ENUM is
       the whole reason it exists. §3.2.18's conversion is ToString-then-membership, and null ToStrings to the
       string "null", which no enumeration lists — so a nullable enumeration declared as IDL_ENUM makes the
       IDL's OWN default value a TypeError. The alternative was IDL_ANY plus the rule written out in the body,
       which is the shape the declared types exist to replace and which here would run ToString on the page's
       value from a plain C body: the getter this engine aborts on, in the one place a page controls. So null
       and undefined are the IDL null and cross as null; anything else is §3.2.18's conversion exactly.
       NAMED RESIDUAL — THIS TYPE STILL CROSSES AN UNKNOWN AND IDL_ENUM NO LONGER DOES. What is not covered:
       a `T?` position handed unknown external input, whose feasible worlds are the N members §3.2.18 lists,
       §3.2.18's own TypeError, AND THE IDL NULL — Web IDL §3.2.20 Nullable types puts that third world ahead
       of the inner type's conversion ("Otherwise, if V is null or undefined, then return the IDL nullable type
       T? value null"), and the test that reaches it is a test of the VALUE, so for a concolic — which wears an
       ordinary Object — it answers `false` from this engine's own value class rather than from the page's
       value. That is one world more than IDL_ENUM has and it is not the same ask, which is why this row is
       stated rather than folded into that one. What the next diff builds: an ask of N+2 completions at the two
       boundaries IDL_ENUM's arm already stands at, with §3.2.20's null as a completion of its own and the
       inner type's N+1 behind it. How its absence would show: `new NavigateEvent(t, {navigationType: cfg.k})`
       places the concolic unconverted where an IDL_ENUM member of the same dictionary now places one of four
       real strings, so one dictionary answers two ways about the same unknown — which is exactly the split
       IDL_BOOLEAN_NO_DEFAULT was in while IDL_BOOLEAN forked, recorded at that row. */
    IDL_ENUM_NULLABLE,
    /* A `(DOMString or Function)` union, which is TimerHandler and nothing else so far: callable crosses as
       itself, anything else is a DOMString. Named for the rule rather than for the member, because the rule is
       what the IDL states. */
    IDL_STRING_UNLESS_CALLABLE,
    /* `boolean` — Web IDL §3.2.3 boolean, whose whole algorithm is "Let x be the result of computing
       ToBoolean(V)" and "Return the IDL boolean value that is the one that represents the same truth value as
       the JavaScript Boolean value x". The conversion runs none of the page's code (ECMAScript §7.1.2
       ToBoolean ( arg ) has no ToPrimitive step); the READ that precedes it is the page's.
       OVER UNKNOWN EXTERNAL INPUT IT IS A FORK, WHICH IS WHY IT IS NOT A CROSSING TYPE. ECMAScript §7.1.2
       ToBoolean ( arg )'s last step is "Return true", so a concolic — which wears an ordinary Object —
       coerces to `true` and nothing says so.
       Crossing does not help here the way it helps a string: a DOMString's bytes are CARRIED to a sink and the
       body asks the value for them, while a boolean's only consumer is CONTROL FLOW, so a crossed one has
       nowhere to go but a `JS_ToBool` in some body that answers `true` for every unknown there has ever been.
       Both truth values are feasible and the member's algorithm observes different worlds for them, so the
       conversion asks §7.1.2 at the BRANCH seam at its own site, where it is one gate with every `if` the
       page writes over the same value — see idl_concolic_rule. */
    IDL_BOOLEAN,
    /* `object?` — §3.2.16's object type with §3.2.20's nullable wrapper. undefined and null become the IDL
       value null; an Object crosses as itself; ANYTHING ELSE IS A TypeError, which is the whole content of the
       type and the reason it cannot be IDL_ANY with a test in the body. The Console Standard §1.1.10's
       `dir(optional any item, optional object? options)` is what declares one: `console.dir(x, 5)` throws in
       every browser, and a body reading `5.foo` instead would answer undefined and print a table nobody asked
       for. Nothing here reads a member, so no page code runs. */
    IDL_OBJECT_NULLABLE,
    /* A `boolean` DICTIONARY MEMBER WITH NO DEFAULT — and the difference from IDL_BOOLEAN is the whole reason
       it exists. §3.2.17 does not set an absent member at all, so "does not exist" and "exists and is false"
       are two states a dictionary can be in, and DOM §4.3.1's `observe` branches on which: `observe(t,
       {attributeFilter:[]})` sets attributes to true and succeeds, while `observe(t, {attributes:false,
       attributeFilter:[]})` is a TypeError at step 5 — the same filter, the same absence of `true`, opposite
       answers. ToBoolean(undefined) is false, so IDL_BOOLEAN folds the two together and can express neither
       step. MutationObserverInit declares four of these (attributes, characterData, attributeOldValue,
       characterDataOldValue) and the two members that DO carry `= false` (childList, subtree) stay
       IDL_BOOLEAN, which is the IDL's own distinction and not a convention.
       WHAT IT DOES NOT DIFFER IN IS THE CONVERSION. A member that is PRESENT is converted by §3.2.3 exactly as
       IDL_BOOLEAN's is, so unknown external input at either type is the same fork at the same seam — see
       idl_concolic_rule, which answers IDL_CONCOLIC_FORKS for both. The distinction this type exists for is
       about `undefined` and about nothing else. */
    IDL_BOOLEAN_NO_DEFAULT,
    /* `sequence<DOMString>` — §3.2.21's iterator-protocol conversion with DOMString as the element type.
       DOM §4.3.1's `attributeFilter` is the first, and it is a DICTIONARY MEMBER: the iterator protocol is the
       page's code at every step, so a member declared this way parks on the element it is on exactly as an
       argument-position sequence does, rather than being walked from a body after every later member was
       already read. */
    IDL_SEQUENCE_DOMSTRING,
    /* `sequence<E>` where E is an ENUMERATION — §3.2.21 Sequences' iterator-protocol conversion with §3.2.18
       Enumeration types as the element conversion. Web Cryptography §14.3.9 The importKey method's
       `sequence<KeyUsage> keyUsages` is the first.
       THE ELEMENT CONVERSION IS TWO STEPS AND THE FIRST OF THEM IS THE PAGE'S CODE. §3.2.18 is "Let S be the
       result of calling ? ToString(V)", then "If S is not one of E's enumeration values, then throw a
       TypeError" — so an element rests on its ToString exactly as IDL_SEQUENCE_DOMSTRING's does, and the
       membership test that follows runs none of the page's code and is decided before the cursor's next pull.
       AND THE PLACE THAT TEST RUNS IS OBSERVABLE, which is the whole reason this is a declared type rather
       than a walk in a body. §3.2.21.1 Creating a sequence from an iterable puts the element conversion INSIDE
       the repeat loop — step 3.1 is "Let next be ? IteratorStepValue(iteratorRecord)" and step 3.3 is
       "Initialize S i to the result of converting next to an IDL value of type T" — so a bogus element at
       index 0 throws BEFORE index 1 is pulled, and a page whose iterator has side effects per element can tell
       that apart from a walk that collects the whole list and checks it afterwards. A body cannot get that
       order back: it runs after §3.6 has converted every position.
       The value list is declared where the POSITION is: beside an argument position by idl_arg_enum, exactly
       as an interface type's is (idl_arg_iface) and a typed array's T is (idl_typed_array), and beside a
       DICTIONARY MEMBER in IdlDictMember::values — the same two roads a bare IDL_ENUM's list takes, because
       the list belongs to the TYPE and both walks convert the same type. */
    IDL_SEQUENCE_ENUM,
    /* `sequence<double>` — §3.2.21 Sequences' iterator-protocol conversion with §3.2.7 `double` as the
       element type.
       Intersection Observer §2.4's `threshold` is the first, reached as the arm of the union below.
       BOTH HALVES RUN THE PAGE'S CODE, which is what makes it a declared type rather than a body's walk: the
       protocol is the page's at every pull, and §3.2.7's element conversion is ToNumber, which is the page's
       `valueOf` — so `{threshold: [{valueOf(){ … }}]}` parks TWICE per element, once on the pull and once on
       the coercion. Both are the same cursor every other sequence uses, so the walk rests on the element it is
       on at whatever depth it is at.
       §3.2.7 `double` REJECTS A NON-FINITE VALUE (it is the RESTRICTED type — §3.2.8 is `unrestricted
       double`), so NaN and the two infinities are a
       TypeError, and that check belongs to the element type rather than to whichever algorithm reads the list
       afterwards. (Intersection Observer §3.2.1 step 6's own RangeError for a value outside [0, 1] is that
       ALGORITHM's and stays there — a different error for a different question.) */
    IDL_SEQUENCE_DOUBLE,
    /* `(DOMString or sequence<DOMString>)` — Web IDL §3.2.25 Union types' union, and the FIRST declared type
       whose ARM IS CHOSEN BY THE PAGE'S OWN CODE. Every other union in this list is decided by a brand test or
       by `JS_IsObject`, neither of which reads anything; this one's step 11.2 is
       `? GetMethod(V, %Symbol.iterator%)` — one accessor or one Proxy `get` trap away from being a page loop —
       so the position PARKS on that read exactly as it parks on a `toString`, and which arm it resolved to is a
       resume point of its own.
       THE ORDER IS THE ALGORITHM'S AND IT IS OBSERVABLE. Steps 4 through 10 name no arm this union has (no
       dictionary, no interface type, no `object`, no buffer source, no callback function), so the whole
       decision is step 11.2 against step 15: an Object whose @@iterator is callable takes the SEQUENCE, and
       EVERYTHING else takes the string arm and is ToString'd — an Object with no @@iterator included, and null,
       and a number. `db.transaction({})` is therefore the store name "[object Object]" and a "NotFoundError",
       which is what a browser answers; reading the union as "an object is the sequence" gets that one wrong in
       the direction of a TypeError the spec does not have. And it is GetMethod and NOT Get: undefined and null
       mean there is no method, and a @@iterator that is neither of those and not callable is a TypeError.
       The element type is a DOMString, so §3.2.12's scalar value conversion does NOT run over it — that is the
       whole of what separates this sequence's elements from `BlobPart`'s. Indexed Database §4.4's
       `transaction(storeNames, …)` is what declares it. */
    IDL_DOMSTRING_OR_SEQUENCE,
    /* `(DOMString or sequence<DOMString>)?` — the same union with §3.2.25 STEP 2 ahead of it: the union
       INCLUDES a nullable type, so null AND undefined are the IDL null and nothing is read off either. The
       difference from the row above is the difference between `keyPath: null` meaning "this store has no key
       path" and meaning the four characters "null", which §2.5 then refuses as an invalid key path.
       Indexed Database §4.4's `IDBObjectStoreParameters.keyPath` declares it, and declares it as a DICTIONARY
       MEMBER — the arm-deciding read is the page's code there exactly as it is at an argument position, so the
       conversion parks on the member it is on rather than being walked from a body after every later member of
       the same dictionary was already read. */
    IDL_DOMSTRING_OR_SEQUENCE_NULLABLE,
    /* `(double or sequence<double>)` — §3.2.25 Union types over the union Intersection Observer §2.4's
       `threshold`
       declares, and its arm is chosen by exactly the read the two rows above are chosen by: §3.2.25 step 11.2's
       `? GetMethod(V, %Symbol.iterator%)`. Nothing in this union names a dictionary, an interface, an `object`,
       a buffer source or a callback, so steps 4 through 10 pass it straight to step 11.2 against step 17 — an
       Object with a CALLABLE @@iterator takes the sequence, and EVERYTHING else takes the numeric arm and is
       ToNumber'd — a Number being answered one clause earlier, by step 13.1's "If types includes a numeric
       type, then return the result of converting V to that numeric type", which is the same type step 17
       names. An Object with no @@iterator included, and null, and a string:
       `{threshold: null}` is
       therefore the number 0 (ToNumber(null)) and passes §3.2.1's range check, while `{threshold: "x"}` is NaN
       and §3.2.7's restricted `double` refuses it as a TypeError.
       IT SHARES THE ARM RESOLVER WITH THE STRING UNION rather than restating step 11.2, because the step is the
       union algorithm's and not the arm list's — a second copy is the second answer that reads @@iterator with
       a different notion of GetMethod. What each union states is only WHICH TWO TYPES its two outcomes are. */
    IDL_DOUBLE_OR_SEQUENCE,
    /* `sequence<T>` where T is an INTERFACE type — §3.2.21's iterator-protocol conversion with §3.2.15's brand
       test as the element conversion. HTML §8.5's `GetHTMLOptions.shadowRoots` is `sequence<ShadowRoot>` and is
       the first, and it is the same reason IDL_SEQUENCE_DOMSTRING is a declared type rather than a body's walk:
       the protocol is the PAGE'S code at every step (the @@iterator read, its call, each `next()`, each
       `done`/`value` read), so the machine parks on the element it is on, and a member driven from a body would
       run it after every later member of the same dictionary was already read.
       The element conversion itself runs NONE of the page's code — §3.2.15 is "if V implements I return it,
       otherwise throw a TypeError" — so it is decided between two pulls of the cursor rather than being a third
       rest point. The interface is named by idl_iface_brand / idl_iface_narrow, exactly as IDL_INTERFACE's is:
       one statement of what the type is, whether it appears alone or inside a sequence. */
    IDL_SEQUENCE_INTERFACE,
    /* `FrozenArray<T>?` / `sequence<T>?` where T is an INTERFACE — §3.2.27 says a frozen array is CONVERTED
       FROM the sequence it is built out of, so an attribute whose IDL type is `FrozenArray<Element>?` takes
       exactly the conversion above under §3.2.20's nullable rule: null and undefined are the IDL null, and an
       object is §3.2.21's iterator protocol with §3.2.15's brand as the element conversion. WAI-ARIA's seven
       `ariaLabelledByElements`-shaped members are the first, and their `?` is what CLEARS them. */
    IDL_SEQUENCE_INTERFACE_NULLABLE,
    /* `sequence<object>` — §3.2.21's iterator-protocol conversion with §3.2.13's `object` as the element type.
       HTML §9.4.4 Message ports' `StructuredSerializeOptions.transfer` is the first — that dictionary is
       declared THERE and not in §2.7 Safe passing of structured data, whose §2.7.6 is StructuredDeserialize —
       and it is what `structuredClone`, `window.postMessage` and `MessagePort.postMessage` all take.
       IT IS A DECLARED TYPE BECAUSE THE WALK IS THE PAGE'S CODE. structured_clone.c converted it from C with a
       `length` read and one indexed read per entry, which is not §3.2.21 at all (that is the array-like
       algorithm) and which runs a getter or a Proxy trap from an activation with no flow base — so
       `structuredClone(v, {transfer: new Proxy([], …)})` reached the page's `get` trap with nothing under it to
       park. The cursor is the same one every other sequence uses, so the conversion rests on the element it is
       on at whatever depth it is at.
       The element conversion runs NONE of the page's code — §3.2.13's `object` is "an Object crosses as itself,
       anything else is a TypeError" — so, like the interface arm, it is decided between two pulls of the cursor
       rather than being a rest point of its own. */
    IDL_SEQUENCE_OBJECT,
    /* `sequence<(DOMString or D)>` where D is a DICTIONARY — §3.2.21's iterator protocol whose ELEMENT type is
       §3.2.25's union of a string and a dictionary. It is the first declared type whose conversion CONTAINS
       another one: an element that is an Object is a dictionary of type D, D's members are read one [[Get]] at
       a time, and one of THOSE members can be a sequence of the same shape again. HTML §8.6.3's SanitizerConfig
       is what declares it — `sequence<SanitizerElementWithAttributes> elements`, each entry
       `(DOMString or SanitizerElementNamespaceWithAttributes)`, whose own `attributes` is
       `sequence<SanitizerAttribute>` — so the conversion is a STACK of cursors, never C recursion: every pull,
       every `done`/`value` read and every member [[Get]] is the page's code and rests where it is, at whatever
       depth it is at. The stack's depth is a property of the DECLARED type tree (which is finite and ends at
       its own leaves), so the pool computes it when the member declares itself and sizes the state for it —
       page data nesting deeper does not make the conversion deeper.
       The dictionary arm is named beside the member (IdlDictMember::dict), which is the other half of what this
       type states, exactly as idl_iface_brand's class is for an interface arm.
       THIS ROW ANSWERS CROSSES WHILE ITS ELEMENT TYPE FORKS, AND THAT IS TWO POSITIONS AND NOT TWO ANSWERS TO
       ONE QUESTION — a distinction worth stating here, because the element IS the `(DOMString or D)` union
       IDL_STRING_OR_DICT declares and a reader who notices that will reach for the rule table. What the rule
       table is keyed by is the type AT A POSITION, and the value at THIS position is the ITERABLE: its
       conversion is §3.2.21, which names no arm at all, so there is nothing here for a fork to be over. What
       an unknown ITERABLE lacks is a LENGTH — §3.2.21.1 Creating a sequence from an iterable repeats until
       step 3.2's `done`, and over an unknown there is no arm set the spec writes down, only an unknown number
       of worlds — which is a different missing capability from an undecided arm and is named as one where it
       is met.
       THE ELEMENT'S OWN ARM IS FORKED, one level in, at the point §3.2.21.1 step 3.3 converts the value the
       cursor just pulled: `{elements: [location.hash]}` has a real Array, a real length and one unknown
       ELEMENT, and that element's union is decided exactly as an argument position's is. See idl_conv_seq_run,
       which is where that ask lives. */
    IDL_SEQUENCE_STRING_OR_DICT,
    /* `(DOMString or D)` where D is a DICTIONARY — §3.2.25 over the union HTML §8.6.2's seven name-taking
       modifiers take (`allowElement(SanitizerElementWithAttributes)` and its six siblings). Its rule is the
       union algorithm's own
       ORDER, and the order is observable: null and undefined take the DICTIONARY arm (step 4, which then throws
       for a `required` member the page did not write), ANY Object takes it too (step 11 — a function and a
       String object included, since these unions name no callback type and step 10's callback clause therefore
       names no entry), and everything else falls through to step 15's string arm. Reading it as "an object is
       the dictionary, a string is the string" agrees on the two ordinary cases and disagrees on
       `allowElement(null)`, which must be a TypeError from the missing `name` rather than the four characters
       "null". The dictionary is named beside the member.
       THE TWO NUMBERS IN THAT SENTENCE WERE 10 AND 12 AND BOTH WERE WRONG, EACH BY A DIFFERENT AMOUNT — step
       10 is "If IsCallable(V) is true" and step 12 is "If V is a Boolean", so the Object clause and the string
       clause were each cited as a neighbour that does something else. It read as authoritative in the one file
       whose whole job is to state a conversion once, and the same pair had been copied into the resolution
       site in idl_args.c. What finds it is counting the algorithm once with LIST DEPTH TRACKED: every one of
       §3.2.25's steps 4 through 14 holds a nested list, so a flat item count promotes their sub-items to peers
       and every number from step 4 onward drifts — which is also why sampling the first number of a cluster
       proves nothing, the drift starting one step AFTER the first nesting rather than at it.
       AND ITS ARM IS FORKED FOR UNKNOWN EXTERNAL INPUT — see idl_concolic_rule, which is where the reason
       lives, and idl_args.c's TWO resolution sites, which is where the fork is asked: the ARGUMENT position,
       and §3.2.21.1 step 3.3's ELEMENT conversion inside IDL_SEQUENCE_STRING_OR_DICT, whose element type is
       this union. One type, one rule, and every site that resolves it asks the same fork.
       NAMED RESIDUAL — A `(D or E)` UNION IS NOT ONE OF THESE, AND THIS ROW USED TO CLAIM THE Sanitizer
       CONSTRUCTOR'S AS AN INSTANCE. `(SanitizerConfig or SanitizerPresets)`'s other arm is an ENUMERATION, and
       Web IDL §2.13 Types puts an enumeration among the STRING TYPES outright — "The string types are
       DOMString, all enumeration types, ByteString and USVString" — so what differs is not the STEP but the
       type step 15 names: this row converts V to §3.2.10 DOMString where that union owes §3.2.18 Enumeration
       types. WHAT IS NOT COVERED is therefore a union whose string arm carries a value list. On a value the
       flow DETERMINED the two agree in what a page can see — a body re-spelling the membership test by hand
       throws the same TypeError one algorithm step later — and over UNKNOWN EXTERNAL INPUT they do not agree
       at all: §3.2.10 CROSSES, so the arm fork PLACES THE UNKNOWN ITSELF, where §3.2.18 forks N+1 ways and
       places one of the declared strings.
       WHAT THE NEXT DIFF BUILDS IS TWO THINGS, AND THE ROW IS THE SMALLER OF THEM. The row is necessary: an
       `IDL_ENUM_OR_DICT` whose dictionary arm is this one's and whose other arm is IDL_ENUM, listed by
       idl_type_has_dict and answering IDL_CONCOLIC_FORKS here, with the value list stated per POSITION by
       idl_arg_enum exactly as a bare IDL_ENUM's is. IT IS NOT SUFFICIENT, and the reason is at the ARGUMENT
       SITE rather than in this rule: that site asks the arm fork afresh on every entry to the position,
       guarded only by the dictionary walk's `started`, because on the string arm NOTHING PARKS — the value is
       placed and the position is done. AN ENUMERATION ARM PARKS. So the resume that carries §3.2.18's own
       answer re-enters, re-asks the ARM fork first, and quickjs-step.h's `fork_ask_key` check refuses it: the
       outstanding answer belongs to the enumeration's question and is being consumed at the union's, which is
       the one thing that check exists to catch. What closes it is a PER-POSITION RECORD of the resolved arm
       that survives a park — what `uni_phase` already is for the @@iterator unions, which is why
       idl_union_seq_arm keeps one and this site does not.
       HOW ITS ABSENCE WOULD SHOW: a page building a sanitizer out of injected state reaches HTML §8.6.2 The
       Sanitizer interface's constructor STILL CARRYING the unknown, so the enumeration's worlds have to be
       asked at the MEMBER's seam by a plain C body that has none — an abort that names a fork at the member
       where the TYPE is what owes it. */
    IDL_STRING_OR_DICT,
    /* THE POSITION AT WHICH TWO OVERLOADS SPLIT, one of them ending here and the other continuing — §3.6's
       resolution algorithm rather than §3.2.25's union, and the difference between the two is why this is its
       own row and not IDL_STRING_OR_DICT with a USVString arm. HTML §7.2.2 The Window object is where the IDL
       that declares it is written (the METHOD STEPS are §9.3.3 Posting messages, which is a different
       section and states no types):

           undefined postMessage(any message, USVString targetOrigin, optional sequence<object> transfer = []);
           undefined postMessage(any message, optional WindowPostMessageOptions options = {});

       §3.6 steps 3-4 come FIRST and they are decided by the ARGUMENT COUNT alone: argcount is
       min(maxarg, args), and every entry whose type list is not that long is removed. The dictionary entry's
       type list ENDS at this position, so a call that passes anything BEYOND it removes that entry outright —
       `postMessage(m, {}, [])` is the three-argument overload, whose second argument is a required USVString,
       and the four characters "[object Object]" are then a "SyntaxError" from the URL parser. Which also means
       this position is REQUIRED at that arity, so its `undefined` is the string "undefined" and not an absent
       optional: the optionality §3.6 step 15.3 reads ("let optionality be the value at index i in the list of
       optionality values of the REMAINING entry") belongs to the entry that SURVIVED step 4, never to the
       declaration as a whole.
       Only once the longer entry is gone does step 12 choose between the two remaining ones, and there the
       rule is IDL_STRING_OR_DICT's own order: null and undefined take the dictionary (step 12.3, whose list of
       qualifying types names "a dictionary type" — and step 12.2 before it for `undefined` alone, since the
       dictionary entry is the one declaring this position optional; step 12.1 is "Let V be args[i]" and
       decides nothing), ANY Object takes it (step 12.11's callback-interface/dictionary/record/object clause),
       and everything else falls through to step 12.15's string clause. `postMessage(m, 123)` is therefore the
       target origin "123", which is a SyntaxError, and not an options dictionary with no members.
       The string arm is a USVString (§3.2.12's scalar value conversion), which is what §7.2.2's IDL writes and
       what every other member of the URL surface takes. The dictionary is named beside the member.
       AND AT THE ARITY WHERE BOTH ENTRIES STAND, THE SURVIVING ENTRY IS FORKED FOR UNKNOWN EXTERNAL INPUT —
       see idl_concolic_rule, which is where the reason lives, and idl_args.c's resolution site, which is where
       the fork is asked. At the LONGER arity there is nothing to fork: steps 3-4 rewrote this position to the
       USVString before any rule was consulted. */
    IDL_USVSTRING_OR_DICT,
    /* THE SAME §3.6 LENGTH-DIFFERING SPLIT WHERE THE LONGER ENTRY'S TYPE AT THIS POSITION IS A NUMBER — and
       where, unlike the row above, THE TWO ENTRIES NEVER COEXIST AT ONE ARITY, so no value is ever looked at.
       CSSOM VIEW §6 Extensions to the Element Interface declares it three times over, and §4 Extensions to the
       Window Interface three more:

           Promise<undefined> scroll(optional ScrollToOptions options = {});
           Promise<undefined> scroll(unrestricted double x, unrestricted double y);

       §3.6's effective overload set for that pair has an entry of length 0, one of length 1 (the dictionary)
       and one of length 2 (the two doubles). Steps 3-4 — argcount is min(maxarg, args), and every entry whose
       type list is not that long is removed — therefore leave EXACTLY ONE entry at every arity, and step 12
       never runs: `el.scrollTo(0, 0)` is the numeric entry because it passed two arguments and
       `el.scrollTo({left: 0})` is the dictionary because it passed one, with nothing about either VALUE
       consulted. Which is also what makes `el.scrollTo(0)` a TypeError — §3.2.17 step 1 refuses a value that is
       not undefined, null or an Object — rather than a scroll to x=0, and that is what a browser answers.
       ITS CONCOLIC RULE IS UNASKED WHERE THE ROW ABOVE'S IS CROSSES, and that is a consequence of the sentence
       above rather than a second policy: by the time idl_concolic_rule is consulted, a call at the LONGER arity
       has already had this position rewritten to the number, so the only value this row itself ever describes
       is a dictionary — a bag of member READS, each yielding another unknown. */
    IDL_UNRESTRICTED_DOUBLE_OR_DICT,
    /* THE SAME §3.6 SPLIT WHERE NEITHER ENTRY IS LONGER — the position the two entries of HTML §9.4.4 Message
       ports' `MessagePort.postMessage` differ at:

           undefined postMessage(any message, sequence<object> transfer);
           undefined postMessage(any message, optional StructuredSerializeOptions options = {});

       IT IS NOT THE ROW ABOVE WITH A SEQUENCE ARM, AND THE DIFFERENCE IS WHICH STEP DECIDES. Both type lists
       are TWO long, so §3.6 step 4 ("remove from S all entries whose type list is not of length argcount")
       removes NEITHER at any arity this member can be called at — the arity shortcut IDL_USVSTRING_OR_DICT
       leans on has nothing to shortcut, and the whole decision is step 12's clause chain at the distinguishing
       argument index. That chain READS THE PAGE'S VALUE, so this position is a rest point.

       EVERY OUTCOME IS ONE OF STEP 12'S CLAUSES, IN THE ALGORITHM'S OWN ORDER:
         - `undefined` — "if V is undefined, and there is an entry in S whose list of optionality values has
           'optional' at index i, then remove from S all other entries". The dictionary entry is the one
           declaring this position optional, so `port.postMessage(m)` and `port.postMessage(m, undefined)` are
           both `options = {}` with every member at its IDL default.
         - `null` — the next clause, "if V is null or undefined, and there is an entry that has … a dictionary
           type": the dictionary again. `port.postMessage(m, null)` therefore transfers nothing rather than
           throwing.
         - an Object whose @@iterator is callable — the SEQUENCE clause, whose test is
           `Let method be ? GetMethod(V, %Symbol.iterator%)`. That is the same operation §3.2.25 step 11.2
           performs, so it is the same read, the same park and the same resolver; step 14 then hands the method
           it found to §3.2.21.1's "creating a sequence from an iterable" rather than reading @@iterator twice.
         - any other Object — the "callback interface type / dictionary type / record type / object" clause:
           the dictionary. `port.postMessage(m, {})` is an options bag, not a zero-length transfer list.
         - EVERYTHING ELSE IS A TypeError, from step 12's final "Otherwise: throw a TypeError". Neither entry
           has a string, numeric, boolean, bigint or `any` type at this position, so no clause below the
           dictionary one names an entry and there is nothing left to select — `port.postMessage(m, "x")`
           THROWS where `window.postMessage(m, "x")` names a target origin. That asymmetry is the whole reason
           this is its own row: an implementation that reuses the string-arm union here invents a transfer list
           out of a value the standard refuses.
       The dictionary is named beside the member, as it is for every row of this shape.
       AND FOR UNKNOWN EXTERNAL INPUT ALL THREE OF THOSE OUTCOMES ARE FORKED — the two entries AND step 12.20's
       TypeError, which is the world a two-armed fork would drop because a concolic wears an ordinary Object
       and every test at the resolution site is written over that Object. See idl_concolic_rule for the reason
       and idl_args.c's resolution site for the ask. */
    IDL_SEQUENCE_OBJECT_OR_DICT,
    /* A DICTIONARY. Web IDL converts one by READING each declared member IN ORDER and converting each by ITS
       OWN type — so a dictionary is that member list plus this very machine, not a second kind of thing. A read
       is one accessor or Proxy trap away from being the page's code, and so is each member's conversion, so
       both are requests exactly like an argument's. The body receives a plain engine-built object carrying the
       converted members, which it reads with an ordinary property get because nothing of the page's is on it.
       The members are declared beside the types — see idl_method_id_dict. */
    IDL_DICT,
    /* `D?` WHERE D IS A DICTIONARY — §3.2.17 under §3.2.20 Nullable types — T?'s rule, and it is a row of its
       own because a nullable dictionary's `null` is the IDL null where a PLAIN dictionary's is a dictionary
       carrying every default. §3.2.20 step 3 is "Otherwise, if V is null or undefined, then return the IDL
       nullable type T? value null" and step 4 is "Otherwise, return the result of converting V using the rules
       for the inner IDL type T"; §3.2.17's own step 1 is "If jsDict is not an Object and jsDict is neither
       undefined nor null, then throw a TypeError", so the un-nullable type ADMITS null and answers a
       defaults-only dictionary for it. Those are two different values and a page distinguishes them.
       Intersection Observer §2.3's `required DOMRectInit? rootBounds` is the member that declares one, and its
       `required` is what makes the third state visible too: for a dictionary member `undefined` IS absent, so
       §3.2.17 step 4.1.6 — "Otherwise, if jsMemberValue is undefined and member is required, then throw a
       TypeError" — refuses `{}` and `{rootBounds: undefined}` alike, while `{rootBounds: null}` is the IDL null
       and `{rootBounds: {}}` is a DOMRectInit whose four members are absent.
       THE DICTIONARY IS NAMED BESIDE THE MEMBER (IdlDictMember::dict), exactly as IDL_DICT's nested form names
       it and as a `sequence<(DOMString or D)>`'s union arm does — one statement of what D is, whether or not
       the member admits null.
       IT IS A DICTIONARY-MEMBER TYPE AND NOT YET AN ARGUMENT ONE, which is a narrowing and not an oversight:
       no member in the platform declares `optional D? x` at a position, and the argument conversion's own
       "an IDL argument was declared with a type this machine does not convert" is what a position declaring it
       would reach — loud, and naming the type. What such a position would additionally need is
       idl_type_is_dictionary's answer for it (§3.6's rule that an omitted DICTIONARY argument is not an absent
       one), and that is a question about ARGUMENTS which this row, on a member, does not raise. */
    IDL_DICT_NULLABLE,
    /* `(AddEventListenerOptions or boolean)` — the one union of this shape in the DOM. Its rule is Web IDL
       §3.2.25 Union types read in the standard's own step order, and the order is the whole of it: step 4 is
       "If V is null or undefined, then:" over a sub-step reading "If types includes a dictionary type, then
       return the result of converting V to that dictionary type.", step 11 sends any other Object there too,
       and ONLY what survives both falls to step 12/18's boolean. So an omitted argument — which §3.6 hands
       this position as `undefined`, because `optional … = {}` makes it a dictionary and not an absence — is
       the EMPTY DICTIONARY and never the boolean `false`.
       THE SENTENCE THAT STOOD HERE — `a value that is NOT an object IS the first declared member's boolean`
       — IS WRONG BY EXACTLY STEP 4, and it is
       rewritten rather than deleted because it is re-derivable from DOM §2.7 Interface EventTarget's flatten
       options alone ("If options is a boolean, then return options") — that algorithm asks about the
       CONVERTED IDL value, so it presupposes §3.2.25 and cannot be read as replacing it. Named for the rule
       rather than for the member, because the rule is what the IDL states — the same reason
       IDL_STRING_UNLESS_CALLABLE is named that way. */
    IDL_DICT_OR_BOOL_FIRST,
    /* `(boolean or ScrollIntoViewOptions)` — §3.2.25's SAME TWO ARMS as the row above with the SAME test, and
       a different destination for the boolean, which is why it is a second row rather than a second caller of
       that one. The row above bakes in DOM §2.7 Interface EventTarget's flatten options ("If options is a
       boolean, then return options" — as the `capture` MEMBER), because that is what DOM's own algorithm does
       with the arm. CSSOM VIEW §6 Extensions to the Element Interface's `scrollIntoView(arg)` reads the
       boolean ITSELF at its step 6 — "Otherwise, if arg is false, then set block to "end"" — and `true` sets
       nothing at all, so there is no member for it
       to be flattened into and inventing one would be a dictionary field no IDL declares.
       WHICH IS WHY §3.2.25 STEP 4 IS OBSERVABLE HERE AND NOT AT THE ROW ABOVE. Both rows send undefined and
       null to the DICTIONARY arm, and for `(AddEventListenerOptions or boolean)` that changes nothing a page
       can see — DOM's flatten leaves `capture` false either way. Here the two arms are two SCROLL POSITIONS:
       the dictionary arm leaves step 2's "start" while the boolean `false` sets step 6's "end", so
       `el.scrollIntoView()` and `el.scrollIntoView(null)` land at opposite ends of the element the day the
       arm is decided by object-ness alone. §6's own steps say so: step 5 is the ScrollIntoViewOptions clause
       and `optional (boolean or ScrollIntoViewOptions) arg = {}` makes the omitted call a dictionary.
       SO THE BOOLEAN ARM PLACES THE BOOLEAN and the dictionary arm places the built dictionary, and the BODY
       tells them apart with `JS_IsBool` — which is §3.2.25's own output ("return the result of converting V to
       boolean" against "return the result of converting V to that dictionary type") rather than a shape test
       this file invented. An unknown external input FORKS, for the row above's reason and at the same site. */
    IDL_BOOL_OR_DICT,
    /* `(T or DOMString)` where T is an INTERFACE type — the union §4.2.4 writes for every member that takes
       "a node or some text", and `el.append('hi')` is the ordinary way to write the second half. Its rule is a
       brand check: an object of the interface's CLASS crosses as itself, anything else is a DOMString. The
       class is declared beside the type, so this file needs to know nothing about what a Node is. */
    IDL_STRING_UNLESS_IFACE,
    /* `(double or T)` WHERE T IS AN INTERFACE TYPE — CSS Typed OM 1 §4.3 Numeric Values:'s
       `typedef (double or CSSNumericValue) CSSNumberish`, which that section introduces with "Any place that
       accepts a CSSNumericValue also accepts a raw double", and `CSS.px(1).equals(2)` is the ordinary way a
       page writes the second half. It is the row directly above with the OTHER arm, and the two are separate
       rows for the reason every pair here is: the arm a value that is NOT the interface takes IS the type.
       §3.2.25 Union types DECIDES IT IN TWO CLAUSES AND THE REST OF THAT ALGORITHM IS SKIPPED BY WHAT THE
       UNION DOES NOT NAME. Its interface clause is reached first — "If V is a platform object, then: If types
       includes an interface type that V implements, then return the IDL value that is a reference to the
       object V" — and every value that is not one falls past every Object clause, because this union names no
       dictionary, no sequence, no record, no callback and no string type, to "If types includes a numeric
       type, then return the result of converting V to that numeric type".
       SO THE OTHER ARM IS §3.2.7's RESTRICTED `double`, AND THAT IS OBSERVABLE AT BOTH ENDS OF THE ALGORITHM.
       §3.2.7 is "Let x be ? ToNumber(V). If x is NaN, +∞, or −∞, then throw a TypeError" — so `equals(null)`
       is a comparison against +0 (no clause above the numeric one names null, and ToNumber(null) is +0) while
       `equals(undefined)` is a TypeError, and a declaration that sorted the two arms in a BODY instead would
       get both of those wrong in one line and would run the page's `valueOf` from a plain C activation
       besides. The unrestricted spelling is not this row: `(unrestricted double or T)` admits a NaN, and no
       member of this platform writes one.
       WHAT UNKNOWN EXTERNAL INPUT DOES HERE IS DECIDED AND NOT FORKED, which is idl_concolic_rule's default
       CROSSES and is stated here because the neighbouring unions are the opposite. §3.2.25's first clause
       asks whether V is a PLATFORM OBJECT IMPLEMENTING T, and a concolic is the solver's own value class and
       implements nothing — so the numeric arm is the arm for every unknown, exactly as it is for every other
       non-T value, and §3.2's numeric boundary then passes the unknown through as itself. The rows above fork
       because their arm asks "is V an Object", which a concolic wears; this one does not ask that.
       The class or predicate is declared beside the type exactly as IDL_STRING_UNLESS_IFACE's is
       (idl_iface_brand for an interface one class names exactly, idl_arg_iface for one it does not), so this
       file needs to know nothing about what a CSSNumericValue is. */
    IDL_DOUBLE_UNLESS_IFACE,
    /* `(object or DOMString)` — Web Cryptography §14's `typedef (object or DOMString) AlgorithmIdentifier`,
       and the only union in this platform whose object arm is the IDL type `object` itself. Its rule is the
       same shape as the two above with a broader test: any Object crosses as itself, and EVERYTHING else —
       null and undefined included — is the DOMString arm.
       IT IS NOT IDL_STRING_OR_DICT AND THE DIFFERENCE IS OBSERVABLE. That type's union names a dictionary, so
       §3.2.25 step 4's clause for null/undefined sends them to the dictionary and a missing `required` member
       is a TypeError; this union names none, so `digest(null, b)` becomes the four characters "null", which
       normalizing an algorithm then reports as a "NotSupportedError" — a different exception, arriving through
       a rejected promise rather than a throw. The dictionary conversion this type does NOT perform is
       §18.4.4's, run by the member's own algorithm at the step the standard numbers it, which is what keeps a
       throwing `name` getter a REJECTION (§14.3.5 step 3) rather than a synchronous TypeError. */
    IDL_STRING_UNLESS_OBJECT,
    /* `BodyInit?` — Fetch's `(ReadableStream or Blob or BufferSource or FormData or URLSearchParams or
       USVString)?`. Its rule is a BRAND check like the two above, but against the BUFFER SOURCE shape rather
       than one class: an ArrayBuffer or any ArrayBufferView crosses as itself, null and undefined are the IDL
       null, and everything else is the union's USVString arm. Blob, FormData, URLSearchParams and
       ReadableStream are brand tests beside it, each asked of the component that owns the interface. The body
       learns nothing either way — §5.1's extraction reads the arm back off the value. */
    IDL_BODYINIT_NULLABLE,
    /* `sequence<BlobPart>` — §3.2.21's iterator-protocol conversion with `(BufferSource or Blob or USVString)`
       as the element type. Named for the IDL type it IS, the way IDL_BODYINIT_NULLABLE is: the union's brand
       test lives in the one place the union is stated, and the member that takes it learns nothing.
       IT IS A DECLARED TYPE and not something a body walks, because Web IDL converts arguments LEFT TO RIGHT —
       a sequence driven from the body runs after every later argument's conversion, which is observable the
       moment a later argument is a dictionary with a getter on it. */
    IDL_SEQUENCE_BLOBPART,
    /* `BufferSource` — §4.2's `typedef (ArrayBufferView or ArrayBuffer) BufferSource`, converted by §3.2.26
       Buffer source types. An ArrayBuffer, a typed array or a DataView crosses as itself and anything else is a
       TypeError, which is a check the body must not make: written by hand it was right twice and wrong the
       third time, where a plain object reached JS_GetArrayBufferView and tripped the engine's own "this is a
       typed array" assertion.
       THE SECTION NUMBER WAS §3.2.25 HERE AND IN THE CONVERSION, AND §3.2.25 IS `Union types`. BufferSource is
       a union, so the wrong number read as plausible for as long as nobody opened it — the failure mode
       CLAUDE.md §Browser half names, where a citation sends the reader to a section that does not say what the
       code claims. The conversion this row performs is §3.2.26's. */
    IDL_BUFFERSOURCE,
    /* `ArrayBufferView` — §4.1's typedef, converted by §3.2.26 Buffer source types. It is the OTHER ARM of the
       union above rather than a narrowing of it, and Web Cryptography §10.1.1 The getRandomValues method is
       what needs it: `ArrayBufferView getRandomValues(ArrayBufferView array)`.
       THE DIFFERENCE IS OBSERVABLE ON THE FIRST LINE A PAGE WRITES. `crypto.getRandomValues(new ArrayBuffer(8))`
       is a TypeError from the CONVERSION; `crypto.getRandomValues(new Float64Array(8))` reaches the algorithm
       and takes §10.1.1 step 1's TypeMismatchError — because §4.1's typedef LISTS Float16Array, Float32Array,
       Float64Array and DataView among the thirteen view types, so the conversion admits exactly what the
       algorithm then refuses. Declaring the member IDL_BUFFERSOURCE would collapse those two into one answer,
       and a feature detection distinguishes them.
       BOTH ROWS ALSO PERFORM §3.2.26's TWO REFUSALS — a shared buffer and a resizable one, neither of which
       §4.1 or §4.2 admits — through idl_buffer_source_refuse. That is where the byte-length hazard of a
       length-tracking view is answered: the conversion keeps one out of every position that did not ask for
       one, rather than each fill site asserting after the fact that the window it was handed still fits. */
    IDL_ARRAYBUFFERVIEW,
    /* ONE OF §3.2.26 Buffer source types' TWELVE TYPED ARRAYS — the arm whose brand test is not "is this a
       view" but "is this THAT view", and the first declared type whose conversion needs a fact no row can
       carry. §3.2.26's typed-array algorithm reads, in this order:
         1. "Let T be the IDL type V is being converted to."
         2. "If V is not an Object, or V does not have a [[TypedArrayName]] internal slot with a value equal to
            T's name, then throw a TypeError."
         3. the [AllowShared] refusal, then 4. the [AllowResizable] one — the same two idl_buffer_source_refuse
            already performs for the two rows above, asked AFTER the brand and never before it.
       SO `T` IS A PARAMETER OF THE CONVERSION AND NOT A ROW OF THIS LIST: twelve rows would state one rule
       twelve times and differ only in a constant, which is the per-member line this file exists to remove.
       The type is declared beside the POSITION instead (idl_typed_array), exactly as
       an interface type's class is (idl_iface_brand) and an enumeration's value list is (idl_arg_enum): one
       row stating the RULE, one declaration stating what this position's rule is about.
       AND THE TWO §3.3 EXTENDED ATTRIBUTES ARE DECLARED WITH IT, because §3.2.26 reads them as CONDITIONS on
       steps 3 and 4 rather than as a different algorithm. §3.3.1 [AllowResizable] and §3.3.2 [AllowShared] are
       independent — §3.3.2's own example writes all four combinations of them on one interface — so they are
       two flags on the position and not a fifth row here, which would have to enumerate the product.
       Encoding §7.4 Interface TextEncoder is the first, and it is why this row exists rather than a test in a
       body: `TextEncoderEncodeIntoResult encodeInto(USVString source, [AllowShared] Uint8Array destination)`.
       A body's own `JS_GetTypedArrayType(argv[1]) != JS_TYPED_ARRAY_UINT8` got step 2 right and asked steps 3
       and 4 NOTHING, so a length-tracking Uint8Array over a resizable buffer reached a write bounded by a byte
       length its buffer no longer had. That is the hazard §4.1's and §4.2's rows already keep out of every
       position that did not ask for one, and it belongs to the TYPE for the same reason: a fill site can only
       assert after the fact that the window it was handed still fits, and such an assert firing is a defect
       that already reached the algorithm. */
    IDL_TYPED_ARRAY,
    /* AN INTERFACE TYPE — §3.2.15. `Node root`, `Range sourceRange`, `Node currentNode`: a platform object
       implementing the interface crosses as itself and ANYTHING else is a TypeError, thrown before the
       algorithm's step 1. It is a declared type rather than a body's `if` for the reason every other brand test
       here is: `walker.currentNode = null` must throw, and a body that checks by hand is a body that can forget
       to. The class is declared beside it with idl_iface_brand, which is what "implementing the interface"
       means to this engine. */
    IDL_INTERFACE,
    /* `T?` WHERE T IS AN INTERFACE — §3.2.15 under §3.2.20's nullable rule: null AND undefined are the IDL
       null, and what survives takes the brand test above. WAI-ARIA's `Element? ariaActiveDescendantElement` is
       the first, and the `?` is the whole of what makes `el.ariaActiveDescendantElement = null` a CLEAR rather
       than the TypeError the un-nullable type owes. The class is named by idl_iface_brand exactly as it is for
       the un-nullable one — one statement of what the interface is, whether or not it is nullable. */
    IDL_INTERFACE_NULLABLE,
    /* A NULLABLE CALLBACK INTERFACE — §3.2.16. `NodeFilter? filter` is the only shape of it here, and its rule
       is not IDL_CALLBACK's: a callback INTERFACE accepts any object (its operation is read off it by name),
       so a non-callable object is valid and only a primitive is a TypeError. null and undefined are the IDL
       null. Declared apart from IDL_CALLBACK because conflating them rejects `{acceptNode(){}}`, which is the
       ordinary way a page writes a filter. */
    IDL_CALLBACK_INTERFACE_NULLABLE,
    /* `(File or USVString or FormData)?` — HTML §4.13.7.3's `setFormValue` arguments, and its rule is
       IDL_BODYINIT_NULLABLE's shape over a different arm list: null and undefined are the IDL null, a File or a
       FormData crosses as itself, and everything else is the USVString arm. A plain BLOB is NOT one of the arms
       — the union names File — so it takes the string arm and stringifies, which is the case a hand-written
       body gets wrong by asking `blob_is`. */
    IDL_FORMVALUE_NULLABLE,
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
static inline bool idl_type_pushes_level(IdlArgType t)
{
    switch (t) {
    case IDL_DICT:
    case IDL_DICT_NULLABLE:
    case IDL_SEQUENCE_STRING_OR_DICT:
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
enum { IDL_FRAME_DICT = 0, IDL_FRAME_SEQUENCE };

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
    uint32_t    n;          /* SEQUENCE only: how many elements `list` holds */
    uint8_t     kind;       /* IDL_FRAME_DICT / IDL_FRAME_SEQUENCE */
    uint8_t     phase;      /* SEQUENCE only */
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
       rather than inside the member's `visit`. NULL for a member that may always be forked, which is every one
       of them but the FRAGMENT PARSE: between two of its one-byte steps it holds a live lxb_html_parser_t, and
       lexbor exposes no copy of one. That declaration names the capability to build, and when it is built this
       field goes with it. */
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
 * every arity is resolved by whichever was found first. */
void idl_overload_length_split_at(int shorter_last_position);

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
void idl_install_value_attribute(JSContext *ctx, JSValueConst target, const char *name, JSValue value,
                                 IdlAttrForge forge);

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
   the parent the IDL names while a page reaches none of them. */
void idl_interface_tag(JSContext *ctx, JSValueConst proto, const char *iface);

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
 * axis (steps 2 and 3); the EXPOSURE-SET axis (step 1) is decided by an identifier and a realm, and neither of
 * those is a thing a component has to state.
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
 * call sites — with the difference that here no caller states anything at all. */
bool idl_exposed_in_realm(JSContext *ctx, const char *identifier);

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
 * AbortSignal's `aborted` and `reason` and AbortController's `signal`, are defined by JS_DefinePropertyGetSet
 * rather than by an installer, so they get §3.7.6's descriptor and its name from their own call site and
 * nothing checks that they agree with the installers.
 *   ALL FOUR ARE READONLY, so the plain-C-SETTER premise this residual was originally written on does not
 * describe a single one of the surviving sites — they have no setter to install. What actually separates them
 * is the GETTER'S C SHAPE against `IdlGetter`, which is `(ctx, this_val, magic)`. `js_template_content` and
 * `js_ctrl_get_signal` ALREADY HAVE EXACTLY THAT SHAPE and are minted JS_CFUNC_getter_magic, so those two can
 * install through `idl_install_accessor(..., getter, 0, -1)` — the readonly form, since a negative setter id
 * mints no setter — with no new mechanism whatever. `js_sig_get_aborted` and `js_sig_get_reason` are
 * JS_CFUNC_generic `(ctx, this_val, argc, argv)` and need that one-line shape change first.
 *   WHAT THE NEXT DIFF BUILDS: nothing in this file. It converts `content` and `signal` to
 * `idl_install_accessor` as they stand, then changes `aborted`/`reason` to the `IdlGetter` shape and does the
 * same, after which this declaration has no callers left and goes.
 *   HOW ITS ABSENCE SHOWS: a member added at a raw site keeps
 * §3.7.6's [[Enumerable]]/[[Configurable]] pair and its name under whoever wrote that line, so it can
 * differ from every installed member without any gate saying so — which is how `content` came to answer
 * `Object.getOwnPropertyDescriptor(HTMLTemplateElement.prototype,"content").get.name` with "content".
 *   AND THE ENUMERATION ABOVE HAS ALREADY BEEN WRONG ONCE, IN THE DIRECTION THAT MAKES THE WORK LOOK SMALLER.
 * It named two sites and there were five: core/dom/abort.c's three were missing from it for as long as it
 * stood, and they are the ones that hand-spell "get aborted"/"get reason"/"get signal" as string literals
 * instead of reaching this composer — so the very defect the clause describes was being committed by sites
 * the clause did not list. Re-derive the list before working from it; it is
 * `grep -rn JS_DefinePropertyGetSet engine/host --include=*.c` minus idl_args.c's own, and that command is
 * the durable half of this paragraph.
 * AND THE DESCRIPTOR IS NO LONGER THE ONLY THING A RAW SITE DECIDES FOR ITSELF. The installers mint every
 * plain-C attribute getter at one point, and that mint is what gives an attribute installed on the realm's
 * [Global] object §3.7.6's opening steps — the receiver resolution, §3.5's "getter" security check and the
 * Window brand. HTML §8.1.8.1's event handlers ARE Window attributes and WERE defined at the raw site, so the
 * whole family was installed on the global without them: `Object.getOwnPropertyDescriptor(window, "onload")
 * .get.call(crossOriginWindowProxy)` answered out of the reading realm where `onload` is absent from HTML
 * §7.2.1.3.1 CrossOriginProperties and a browser throws "SecurityError". That family now installs through
 * `idl_install_accessor_step`, which states §3.5's kind at the mint; the four remaining raw sites still do
 * not, and none of them is on a [Global] object, which is why this is the weaker half of their absence. */
#define IDL_ACCESSOR_NAME_MAX 96
typedef enum { IDL_ACCESSOR_GET, IDL_ACCESSOR_SET } IdlAccessorKind;
const char *idl_accessor_name(char *buf, size_t cap, const char *id, IdlAccessorKind kind);

/* AN ATTRIBUTE THAT STATES ITS IDL'S EXPOSURE. This is the general form; the plain `idl_install_accessor`
   below is the same install for a member whose IDL carries no exposure condition, which is most of them. */
void idl_install_accessor_exposed(JSContext *ctx, JSValueConst target, const char *name,
                                  IdlGetter getter, int getter_magic, int setter_stepid, IdlExposure exposure);

void idl_install_accessor(JSContext *ctx, JSValueConst target, const char *name,
                          IdlGetter getter, int getter_magic, int setter_stepid);

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
void idl_install_accessor_no_user_code(JSContext *ctx, JSValueConst target, const char *name,
                                       IdlGetter getter, int getter_magic, int setter_stepid);

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
void idl_install_accessor_unforgeable(JSContext *ctx, JSValueConst target, const char *name,
                                      IdlGetter getter, int getter_magic, int setter_stepid);

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
   object. The interface branded against is Window, and that is ASSERTED at the install rather than assumed at
   the read: `target` must be the realm's global object.
   THE `IdlGetter` FORM'S GETTER STEPS STILL SEE AN UNRESOLVED RECEIVER, because §3.7.6's check belongs to the
   mint and a raw C getter cannot be wrapped without carrying a function pointer through the closure's data —
   which JSCFunctionType must never hold. Those getters answer per-realm and so answer correctly; what they do
   not do is THROW for a receiver that implements nothing. See the note on the resolution in idl_args.c. */
void idl_install_replaceable(JSContext *ctx, JSValueConst target, const char *name,
                             IdlGetter getter, int getter_magic);
/* The half of that setter a member with its OWN setter steps still needs: Web IDL's
   CreateDataPropertyOrThrow(receiver, name, V), which REPLACES the accessor on that object. HTML §7.2.5's
   `opener` setter ends in exactly this operation for a non-null value, so it is one implementation reached from
   two declarations rather than two that can drift. Returns <0 with an exception pending, like every define. */
int  idl_replace_with_value(JSContext *ctx, JSValueConst obj, const char *name, JSValueConst v);
void idl_install_replaceable_value(JSContext *ctx, JSValueConst target, const char *name, JSValue value);

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
   all — suspends and resumes at the exact argument it was on.
   THERE IS NO `length` TO PASS, AND THAT IS THE POINT. Web IDL §3.7.7 Operations' length is "the length of the
   shortest argument list in the entries in S" over the effective overload set at argument count 0, which is a
   function of the DECLARATION alone — min(first optional, declared positions) — so this pool computes it and
   an install has nothing to remember. It used to be a parameter, and the seven installs of DOM §4.2.6 Mixin
   ParentNode's and §4.2.8 Mixin ChildNode's members, all reaching ONE declaration, DISAGREED WITH EACH OTHER
   about it: five said 1 and two said 0 for the identical arity, so `Element.prototype.append.length` was a
   number no reading of the IDL produces. `new Event()`'s interface object had the same defect one section over,
   carrying the declared arity 2 where §3.7.1 Interface object computes 1. See idl_member_length_of. */
void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int stepid);
/* §3.4.10's [LegacyUnforgeable] FOR AN OPERATION — the twin of idl_install_accessor_unforgeable, and it goes
   through §3.7.7's own descriptor: on the INSTANCE the caller passes rather than on a prototype, and
   {[[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false}. Two installers because §3.7.7 states
   two, not because a caller may pick. */
void idl_install_method_unforgeable(JSContext *ctx, JSValueConst target, const char *name, int stepid);
/* THE SAME INSTALL FOR A METHOD THAT STATES ITS IDL'S EXPOSURE — §3.3.13's [SecureContext] REMOVES the member,
   for an operation exactly as for an attribute, and this is that one rule asked in the one place. */
void idl_install_method_exposed(JSContext *ctx, JSValueConst target, const char *name, int stepid,
                                IdlExposure exposure);
/* The installer for a method whose algorithm is a step machine of its OWN (its own JSTrampStepDef) rather than
   a member of the args machine — `click` and `dispatchEvent` today. Separate from idl_install_method because
   they are separate things, and each asserts it was handed its own kind. */
void idl_install_step_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid);

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

#endif
