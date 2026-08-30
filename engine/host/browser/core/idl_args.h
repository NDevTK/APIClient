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
    /* `[EnforceRange] unsigned long` — §3.2.4.10, which REPLACES the modulo with a REFUSAL: a non-finite value,
       or one whose integer part is outside the type's range, is a TypeError instead of a wrap. It is a separate
       type and not a flag for the same reason [Clamp] is: the extended attribute IS the conversion.
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
    /* AN ENUMERATION — §3.2.18. ToString, and then the result must be one of the values the IDL lists or it is
       a TypeError: `new Blob([], {endings: "bogus"})` throws, and an unrecognised value is never silently the
       default. The values are declared beside the member, because they are part of the type. */
    IDL_ENUM,
    /* A NULLABLE ENUMERATION — `NavigationType? navigationType = null`, and the difference from IDL_ENUM is
       the whole reason it exists. §3.2.18's conversion is ToString-then-membership, and null ToStrings to the
       string "null", which no enumeration lists — so a nullable enumeration declared as IDL_ENUM makes the
       IDL's OWN default value a TypeError. The alternative was IDL_ANY plus the rule written out in the body,
       which is the shape the declared types exist to replace and which here would run ToString on the page's
       value from a plain C body: the getter this engine aborts on, in the one place a page controls. So null
       and undefined are the IDL null and cross as null; anything else is §3.2.18's conversion exactly. */
    IDL_ENUM_NULLABLE,
    /* A `(DOMString or Function)` union, which is TimerHandler and nothing else so far: callable crosses as
       itself, anything else is a DOMString. Named for the rule rather than for the member, because the rule is
       what the IDL states. */
    IDL_STRING_UNLESS_CALLABLE,
    IDL_BOOLEAN,          /* ToBoolean. The conversion runs nothing; the READ that precedes it is the page's */
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
       IDL_BOOLEAN, which is the IDL's own distinction and not a convention. */
    IDL_BOOLEAN_NO_DEFAULT,
    /* `sequence<DOMString>` — §3.2.21's iterator-protocol conversion with DOMString as the element type.
       DOM §4.3.1's `attributeFilter` is the first, and it is a DICTIONARY MEMBER: the iterator protocol is the
       page's code at every step, so a member declared this way parks on the element it is on exactly as an
       argument-position sequence does, rather than being walked from a body after every later member was
       already read. */
    IDL_SEQUENCE_DOMSTRING,
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
    /* `(DOMString or sequence<DOMString>)` — §3.2.25's union, and the FIRST declared type whose ARM IS CHOSEN
       BY THE PAGE'S OWN CODE. Every other union in this list is decided by a brand test or by `JS_IsObject`,
       neither of which reads anything; this one's step 11.2 is `? GetMethod(V, %Symbol.iterator%)` — one
       accessor or one Proxy `get` trap away from being a page loop — so the position PARKS on that read exactly
       as it parks on a `toString`, and which arm it resolved to is a resume point of its own.
       THE ORDER IS THE ALGORITHM'S AND IT IS OBSERVABLE. Steps 4 through 11 name no arm this union has (no
       dictionary, no interface type, no `object`, no buffer source, no callback function), so the whole
       decision is step 12.2 against step 16: an Object whose @@iterator is callable takes the SEQUENCE, and
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
       a buffer source or a callback, so steps 4 through 11 pass it straight to step 12.2 against step 14 — an
       Object with a CALLABLE @@iterator takes the sequence, and EVERYTHING else takes the numeric arm and is
       ToNumber'd. An Object with no @@iterator included, and null, and a string: `{threshold: null}` is
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
       type states, exactly as idl_iface_brand's class is for an interface arm. */
    IDL_SEQUENCE_STRING_OR_DICT,
    /* `(DOMString or D)` where D is a DICTIONARY — §3.2.25 over the union HTML §8.6.2's seven name-taking
       modifiers take (`allowElement(SanitizerElementWithAttributes)` and its six siblings), and the union the
       Sanitizer constructor's `(SanitizerConfig or SanitizerPresets)` is. Its rule is the union algorithm's own
       ORDER, and the order is observable: null and undefined take the DICTIONARY arm (step 4, which then throws
       for a `required` member the page did not write), ANY Object takes it too (step 10 — a function and a
       String object included, since these unions name no callback type), and everything else falls through to
       step 12's string arm. Reading it as "an object is the dictionary, a string is the string" agrees on the
       two ordinary cases and disagrees on `allowElement(null)`, which must be a TypeError from the missing
       `name` rather than the four characters "null". The dictionary is named beside the member. */
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
       rule is IDL_STRING_OR_DICT's own order: null and undefined take the dictionary (step 12.2 — and step
       12.1 before it, since the dictionary entry is the one declaring this position optional), ANY Object
       takes it (the callback-interface/dictionary/record clause), and everything else falls through to the
       string clause. `postMessage(m, 123)` is therefore the target origin "123", which is a SyntaxError, and
       not an options dictionary with no members.
       The string arm is a USVString (§3.2.12's scalar value conversion), which is what §7.2.2's IDL writes and
       what every other member of the URL surface takes. The dictionary is named beside the member. */
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
       The dictionary is named beside the member, as it is for every row of this shape. */
    IDL_SEQUENCE_OBJECT_OR_DICT,
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
    /* `(boolean or ScrollIntoViewOptions)` — §3.2.25's SAME TWO ARMS as the row above with the SAME test, and
       a different destination for the boolean, which is why it is a second row rather than a second caller of
       that one. The row above bakes in DOM §2.7 "Interface EventTarget"'s flatten options ("if options is a
       boolean, then return options" — as the `capture` MEMBER), because that is what DOM's own algorithm does
       with the arm. CSSOM VIEW §6's `scrollIntoView(arg)` reads the boolean ITSELF at its step 6 — "otherwise,
       if arg is false, then set block to 'end'" — and `true` sets nothing at all, so there is no member for it
       to be flattened into and inventing one would be a dictionary field no IDL declares.
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
    /* `(object or DOMString)` — Web Cryptography §14's `typedef (object or DOMString) AlgorithmIdentifier`,
       and the only union in this platform whose object arm is the IDL type `object` itself. Its rule is the
       same shape as the two above with a broader test: any Object crosses as itself, and EVERYTHING else —
       null and undefined included — is the DOMString arm.
       IT IS NOT IDL_STRING_OR_DICT AND THE DIFFERENCE IS OBSERVABLE. That type's union names a dictionary, so
       §3.2.26's step for null/undefined sends them to the dictionary and a missing `required` member is a
       TypeError; this union names none, so `digest(null, b)` becomes the four characters "null", which
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
       an interface type's class is (idl_iface_brand) and an enumeration's value list is (idl_enum_values): one
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
    /* §3.2.25's ARM IS A TEST OF THE VALUE — step 11 "If V is an Object" against step 12 "If V is a Boolean"
       and step 18 "If types includes boolean". Over unknown input BOTH clauses are feasible, and the arms
       differ in what the member's own algorithm then observes, so the arm is neither crossed nor picked: it is
       an OUTCOME FORK, asked at the type's own resolution site through the conversion machine's
       step_fork_run, so both worlds run. A type is only ever this when the SITE that resolves it asks that
       fork — the two assert against each other. */
    IDL_CONCOLIC_FORKS,
} IdlConcolicRule;

static inline IdlConcolicRule idl_concolic_rule(IdlArgType t)
{
    switch (t) {
    case IDL_ANY:
    case IDL_DICT:
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
        return IDL_CONCOLIC_FORKS;
    default:
        return IDL_CONCOLIC_CROSSES;
    }
}

/* A DICTIONARY MEMBER, as its IDL declares it: the name, the type of its value, and whether the IDL marks it
   `required` (an absent required member is a TypeError, and for a dictionary `undefined` IS absent). A member
   with no `required` written is optional, which is what leaving the field off an initialiser gives. */
/* `values` is the NULL-terminated list an IDL_ENUM member's IDL lists, and is read by nothing else.
   `level` is WHICH DICTIONARY IN THE INHERITANCE CHAIN declares the member — 0 for the most-derived one's
   BASE, counting up to the dictionary itself. §3.2.17 reads the INHERITED members first and each dictionary's
   own members lexicographically among themselves, so `FilePropertyBag : BlobPropertyBag` reads endings, type,
   then lastModified — an order no single sorted list produces, because `lastModified` sorts before `type`.
   Stating the level is what lets the declaration express that AND still be checkable. */
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
} IdlDictMember;

/* A DICTIONARY, DECLARED — its member list in §3.2.17's read order, and the identifier its IDL gives it. A
   member's OWN dictionary argument is declared as the bare list (idl_method_id_dict); a NESTED one needs that
   list NAMED, because the type that reaches it is stated on the member that holds it and a conversion
   diagnostic has to be able to say which dictionary refused a value. */
typedef struct IdlDictDecl {
    const char          *name;
    const IdlDictMember *members;
    int                  n;
} IdlDictDecl;

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
    /* WHAT THE DECLARATION CANNOT NAME, and nothing else: a lexbor handle, a foreign C allocation, a global or
       per-object FLAG the algorithm took and must give back on every exit (§4.13.4 step 14's "regardless of
       whether the above steps threw", HTML §4.10.22.3 step 8's constructing-entry-list flag). It runs BEFORE
       the declaration is discharged, so it may READ an owned value — those flags live on one — and idl_args.c
       asserts across the call that it FREED none. A member with nothing of that kind declares NULL. */
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

/* §3.6 STEP 14.2'S DEFAULT VALUE AT A POSITIONAL ARGUMENT — the THIRD state at a position, beside "the page
   passed one" and "the argument is absent", and exactly the distinction IdlDictDefault already draws for a
   dictionary member. §3.6's absent rule above is for an optional argument with NO default value; where the
   IDL writes `= …`, step 14.2 replaces the undefined with THAT value and the body never sees a hole.
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
   idl_iface_brand and idl_optional_from do. */
void idl_iface_narrow(bool (*is)(JSValueConst v));

/* DECLARE THE VALUES AN IDL_ENUM POSITION ADMITS — §3.2.18's enumeration, whose value list IS the type. A
   NULL-terminated array of the identifiers the IDL lists, which the conversion checks the string ToString
   produced against and throws a TypeError for anything else.
   IT WAS EXPRESSIBLE ONLY ON A DICTIONARY MEMBER, and the conversion stood at a DCHECK saying so: "an
   ENUMERATION was declared as a positional argument — the value list lives on a dictionary member … give the
   declaration somewhere to carry the list". HTML §7.2.5's `attribute ScrollRestoration scrollRestoration` is
   that position — a setter, one value, no dictionary — and `history.scrollRestoration = "bogus"` is a
   TypeError from the TYPE rather than from the algorithm, which is why the check belongs here and not in the
   setter's body. Set after the declaration, naming the member the LAST one made, as idl_iface_brand and
   idl_optional_from do; `values` must outlive the declaration, so every caller passes a static. */
void idl_enum_values(const char *const *values);

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
   and idl_enum_values do. idl_args_seal asserts BOTH directions: a position declared IDL_TYPED_ARRAY that
   states no T is a conversion that cannot start, and a T stated at a position of any other type is a
   declaration describing a member that is not this one. */
void idl_typed_array(int index, JSTypedArrayEnum kind, bool allow_shared, bool allow_resizable);

/* DECLARE THAT THIS MEMBER'S TAIL IS VARIADIC — `T... name`, so the LAST declared type applies to every
   argument from that position on and the member takes as many as the page passed.
   IT IS SET AFTER THE DECLARATION, naming the member the LAST one made, exactly as idl_optional_from,
   idl_arg_default, idl_iface_brand and idl_enum_values do. It existed only as a parameter of
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
 * idl_iface_brand, idl_enum_values and idl_variadic do, and for the same reason: the id a declaration returns
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
   INSERTION STEPS with "These steps must not modify the node tree that insertedNode participates in, fire
   events, or otherwise execute JavaScript", so a request that parked on the page's code between two of them
   would be a timeline the standard forbids. Its POST-CONNECTION steps are the opposite by construction —
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
/* §3.11.1's LEGACY CALLBACK INTERFACE OBJECT — what a callback interface on which constants are defined puts
   on the global. It is a BUILT-IN FUNCTION OBJECT ("Let F be CreateBuiltinFunction(steps, 0, id, « », realm)"
   over steps that throw a TypeError), which is why the spec's own note says `typeof` answers "function"; an
   ordinary object answers "object", and that is a fact a page reads. A callback interface has NO interface
   prototype object, so there is no §3.7.3 tag anywhere on it and this call is the only statement of which
   interface the constants installed on the returned object belong to. */
JSValue idl_callback_interface_object(JSContext *ctx, const char *name);

JSValue idl_step_function(JSContext *ctx, const char *name, int length, int stepid);
JSValue idl_step_constructor(JSContext *ctx, const char *name, int length, int stepid);

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

/* WEB IDL §3.3.7 [Exposed]'s EXPOSURE CONDITIONS — the extended attributes that decide whether a member EXISTS
 * in a realm, as opposed to what it answers. The "is exposed in realm" algorithm defined under that heading is
 * four steps and this is the one of them that is not about which global the member is on. (It read §3.9 here
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
 * THE OTHER TWO §3.3.7 CONDITIONS ARE HONESTLY ABSENT, and their absence is not a default. [Exposed] is decided
 * by WHICH global a component installs on and this engine has exactly one global kind (there is no
 * WorkerGlobalScope), so every member's exposure set is trivially satisfied. [CrossOriginIsolated] is decided
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

/* AN ATTRIBUTE THAT STATES ITS IDL'S EXPOSURE. This is the general form; the plain `idl_install_accessor`
   below is the same install for a member whose IDL carries no exposure condition, which is most of them. */
void idl_install_accessor_exposed(JSContext *ctx, JSValueConst target, const char *name,
                                  IdlGetter getter, int getter_magic, int setter_stepid, IdlExposure exposure);

void idl_install_accessor(JSContext *ctx, JSValueConst target, const char *name,
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
   all — suspends and resumes at the exact argument it was on. */
void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid);
/* §3.4.10's [LegacyUnforgeable] FOR AN OPERATION — the twin of idl_install_accessor_unforgeable, and it goes
   through §3.7.7's own descriptor: on the INSTANCE the caller passes rather than on a prototype, and
   {[[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false}. Two installers because §3.7.7 states
   two, not because a caller may pick. */
void idl_install_method_unforgeable(JSContext *ctx, JSValueConst target, const char *name, int length,
                                    int stepid);
/* THE SAME INSTALL FOR A METHOD THAT STATES ITS IDL'S EXPOSURE — §3.3.13's [SecureContext] REMOVES the member,
   for an operation exactly as for an attribute, and this is that one rule asked in the one place. */
void idl_install_method_exposed(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid,
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
