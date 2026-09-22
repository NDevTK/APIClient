/* CSS FONT LOADING §2 "The FontFace Interface" and §2.1 "The Constructor". See font_face.h for why this
 * interface, what the corpus sites read off it, and the ORDER the rest of that standard lands in. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/css/css_style_declaration.h"
#include "core/fonts/font_face.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"

/* §2's ELEVEN WRITABLE ATTRIBUTES IN THE ORDER THE IDL DECLARES THEM, FOLLOWED BY `status`. The enumerator IS
   the index into the state Array, IS the accessor's magic and IS the index into both tables below, so a name
   installed with no case to answer it is the one way this can go wrong — which is what the getter's own assert
   is, and what the two length assertions in font_face_init are. */
typedef enum {
    FF_FAMILY = 0,
    FF_STYLE,
    FF_WEIGHT,
    FF_STRETCH,
    FF_UNICODE_RANGE,
    FF_FEATURE_SETTINGS,
    FF_VARIATION_SETTINGS,
    FF_DISPLAY,
    FF_ASCENT_OVERRIDE,
    FF_DESCENT_OVERRIDE,
    FF_LINE_GAP_OVERRIDE,
    FF_ATTR_COUNT,
    /* NOT AN ATTRIBUTE OF THE ELEVEN — a slot of the same Array, holding §2's `readonly attribute
       FontFaceLoadStatus status`. It is past FF_ATTR_COUNT because the eleven above are the ones the
       constructor PARSES and the ones the setters write, and `status` is neither. */
    FF_STATUS = FF_ATTR_COUNT,
    /* §2.1's `[[Urls]]` and `[[Data]]` internal slots, of which "one is null and the other is not null (the
       non-null one is set by the constructor, based on which data is passed in)". ONE slot here, because the
       value itself says which it is — a String is [[Urls]] and a BufferSource is [[Data]] — and two slots
       would be one fact in two places, free to disagree about which is null. */
    FF_SOURCE,
    FF_SLOT_COUNT
} FontFaceSlot;

/* THE ATTRIBUTE'S IDL IDENTIFIER, which is what a page reads it by. */
static const char *const FF_ATTR_ID[FF_ATTR_COUNT] = {
    "family", "style", "weight", "stretch", "unicodeRange", "featureSettings",
    "variationSettings", "display", "ascentOverride", "descentOverride", "lineGapOverride",
};

/* THE @font-face DESCRIPTOR EACH ATTRIBUTE IS PARSED AS — css-fonts-4 §4.2 and the sections after it, named
   here because §2.1's parse is stated in terms of them: "Parse the family argument, and the members of the
   descriptors argument, according to the grammars of the corresponding descriptors of the CSS @font-face
   rule." The PAIRING is the whole content of this table, and it is not derivable from the attribute name —
   `family` is the `font-family` descriptor and `unicodeRange` is `unicode-range`. */
static const char *const FF_ATTR_DESCRIPTOR[FF_ATTR_COUNT] = {
    "font-family", "font-style", "font-weight", "font-stretch", "unicode-range", "font-feature-settings",
    "font-variation-settings", "font-display", "ascent-override", "descent-override", "line-gap-override",
};

/* `dictionary FontFaceDescriptors`'s DEFAULTS, verbatim from the IDL. They are load-bearing rather than
   decoration: a corpus bundle compares a face's eight descriptor attributes against a literal object holding
   exactly these strings, and another splits `unicodeRange` on a regular expression — so a face constructed
   with no descriptors must read these back or both sites get a wrong answer from a member that answered.
   `family` has NO default and is not in this table's business: it is the constructor's first REQUIRED
   argument. Its slot is written unconditionally, which is why the table's first entry is never read and is
   the empty string rather than a plausible family name somebody could come to rely on. */
static const char *const FF_ATTR_DEFAULT[FF_ATTR_COUNT] = {
    "", "normal", "normal", "normal", "U+0-10FFFF", "normal",
    "normal", "auto", "normal", "normal", "normal",
};

/* `enum FontFaceLoadStatus { "unloaded", "loading", "loaded", "error" };` — the two values §2.1 can write. */
#define FF_STATUS_UNLOADED "unloaded"
#define FF_STATUS_ERROR    "error"

static JSClassID g_class;
static JSValue   g_key = JS_UNDEFINED;      /* the face's own state slot */
static JSAtom    g_atom = JS_ATOM_NULL;
static int       g_id_ctor = -1;
static int       g_id_set[FF_ATTR_COUNT];
static int       g_ready;

/* ---- the record ------------------------------------------------------------------------------------------- */

/* The face's backing Array. OWNED. JS_EXCEPTION with a TypeError pending for a receiver that is not a face,
   which is Web IDL §3.7's implementation-check and NOT an assert: a receiver is PAGE-SUPPLIED INPUT, so a
   DCHECK on it would hand any page an abort switch for this engine. */
static JSValue ff_state(JSContext *ctx, JSValueConst obj)
{
    JSValue s;

    if (JS_GetClassID(obj) != g_class)
        return JS_ThrowTypeError(ctx, "not a FontFace");
    if (JS_GetOwnSlot(ctx, &s, obj, g_atom) <= 0)
        s = JS_UNDEFINED;
    DCHECK(JS_IsObject(s),
           "a FontFace carries no state — every one this engine mints has all of its slots written before the "
           "object exists, so one without them was made somewhere else");
    return s;
}

bool font_face_is(JSValueConst v)
{
    return g_class != 0 && JS_GetClassID(v) == g_class;
}

/* ---- CSS FONT LOADING §2.1 "The Constructor"'s PARSE --------------------------------------------------
 *
 * "Parse the family argument, and the members of the descriptors argument, according to the grammars of the
 * corresponding descriptors of the CSS @font-face rule. ... If any of them fail to parse correctly, reject font
 * face's [[FontStatusPromise]] with a DOMException named "SyntaxError", set font face's corresponding attributes
 * to the empty string, and set font face's status attribute to "error". Otherwise, set font face's corresponding
 * attributes to the serialization of the parsed values."
 *
 * IT IS ASKED OF THE COMPONENT THAT OWNS THE @font-face GRAMMAR AND NOT RE-STATED HERE. core/css/
 * css_style_declaration.h's `CSSOM_BLOCK_FONT_FACE` is that grammar — it is the context css_rule.c already
 * serializes an `@font-face` rule's body through — so one declaration text carries one descriptor to it and
 * what comes back is either that descriptor's SERIALIZATION or nothing at all. A second parser written here
 * would be the second copy of a grammar whose first copy is the one a stylesheet is read by, and the two would
 * be free to disagree about the same bytes on the same page.
 *
 * WHAT AN UNTYPED DESCRIPTOR DOES, WHICH IS THE PART A READER MUST NOT GUESS. css_style_declaration.c's
 * collector keeps a longhand whose name it does not VALIDATE with its value stored verbatim — the
 * `!css_shorthand_validates_longhand(name)` arm — so a descriptor core/css/ does not type passes through
 * rather than being dropped, and §2.1's error arm does not fire for it. That is a MORE PERMISSIVE engine and
 * not a wrong value: the attribute reads back what the page wrote, which is what the serialization of a
 * correctly-parsed value would have been for every value that is in fact valid. Which descriptors those are is
 * a fact about core/css/ and moves as that component grows, so it is a command and not a list here:
 *     git grep -n 'css_shorthand_validates_longhand' engine/host/browser/core/css/
 *     git grep -c '"font-display"\|"ascent-override"' engine/host/browser/core/css/
 * A descriptor named ONLY by css_style_declaration.c's FONT_FACE_DESCRIPTORS[] table is one with no value
 * grammar behind it yet.
 *
 * NAMED RESIDUAL — THE ERROR ARM SETS THE ATTRIBUTES AND THE STATUS AND REJECTS NOTHING.
 *   WHAT IS NOT COVERED: §2.1's "reject font face's [[FontStatusPromise]] with a DOMException named
 *     "SyntaxError"". That slot is not on this record, because §2.2 "The load() method" and the `loaded`
 *     attribute that reflects it are the LOADING half and are absent here — a promise created with no member
 *     able to reach it would be an observable this engine invented rather than one §2 gives a page.
 *   WHAT THE NEXT DIFF BUILDS: the slot, `loaded`, and §2.2's `load()`, together — landing (3) in font_face.h's
 *     ORDER, which is where the rest of the loading half is.
 *   HOW ITS ABSENCE WOULD SHOW: `new FontFace("x", "not-a-src").loaded` is undefined rather than a promise, so
 *     a page that awaits it throws a TypeError at its own line; and nothing anywhere fires an unhandled
 *     rejection for a face that failed to parse, where a browser fires one. */

/* The serialization of ONE descriptor's value under the @font-face grammar, or NULL if it declares nothing —
 * which is §2.1's "fail to parse correctly" for that descriptor. OWNED (free()).
 *
 * The block that comes back is CSSOM §6.6's serialization of a one-declaration block, so it is
 * `<name>: <value>;`. The value is what follows the FIRST colon and precedes the LAST semicolon, which is
 * unambiguous because exactly one declaration went in — and the assert below is what keeps that true rather
 * than assumed, over a value this file composed and a serializer this codebase owns. */
static char *ff_parse_descriptor(const char *descriptor, const char *value)
{
    size_t dlen = strlen(descriptor), vlen = strlen(value), n;
    char *decl, *block, *out;
    const char *colon, *semi;

    decl = malloc(dlen + 2 + vlen + 1);
    CHECK(decl != NULL, "css-font-loading: OOM composing an @font-face descriptor declaration");
    memcpy(decl, descriptor, dlen);
    decl[dlen] = ':';
    decl[dlen + 1] = ' ';
    memcpy(decl + dlen + 2, value, vlen);
    decl[dlen + 2 + vlen] = '\0';
    block = cssom_serialize_declarations(decl, dlen + 2 + vlen, CSSOM_BLOCK_FONT_FACE);
    free(decl);
    if (!block) return NULL;          /* the block declares nothing — the value is out of the grammar */

    colon = strchr(block, ':');
    semi = strrchr(block, ';');
    DCHECK(colon != NULL && semi != NULL && semi > colon &&
           strncmp(block, descriptor, dlen) == 0,
           "CSSOM §6.6's serialization of a ONE-declaration @font-face block came back in a shape this file "
           "cannot read a value out of — one declaration went in, so what comes back is `<name>: <value>;` and "
           "nothing else; a block that is not that is this file and core/css/css_style_declaration.c "
           "disagreeing about the grammar they share");
    colon++;
    while (*colon == ' ') colon++;
    n = (size_t)(semi - colon);
    out = malloc(n + 1);
    CHECK(out != NULL, "css-font-loading: OOM copying a serialized @font-face descriptor value");
    memcpy(out, colon, n);
    out[n] = '\0';
    free(block);
    return out;
}

/* §2.1's parse for ONE slot: place the SERIALIZATION of `v` parsed as `slot`'s descriptor, or report the parse
 * failure. Returns false on failure, having placed nothing.
 *
 * UNKNOWN EXTERNAL INPUT IS PLACED AS ITSELF AND IS NOT PARSED, which is a decision and not an omission. A
 * concolic carries a DOMAIN and an EXAMPLE and no bytes this file may treat as the value — §Run-don't-match's
 * rule that a modelable value is never collapsed to bare-concrete — so parsing "its" text would be parsing the
 * shape and then storing a string the page never wrote, which deletes the provenance every branch downstream
 * of `fontFace.family` needs. The non-throwing arm is taken for the reason core/css/css_keyword_value.c gives
 * at its own constructor: refusing an unknown DELETES a world nothing contradicted, and a page's
 * `new FontFace(cfg.family, ...)` denotes a real family in every run. */
static bool ff_place_parsed(JSContext *ctx, JSValue state, int slot, JSValueConst v)
{
    const char *raw;
    char *ser;

    DCHECK(slot >= 0 && slot < FF_ATTR_COUNT,
           "§2.1's parse was asked for a slot that is not one of the eleven attributes it writes — the "
           "enumerator IS the index into the descriptor table, so an unknown one means a name reached this "
           "file with no descriptor to be parsed as");
    if (concolic_is(v)) {
        JS_SetPropertyUint32(ctx, state, (uint32_t)slot, JS_DupValue(ctx, v));
        return true;
    }
    raw = JS_ToCString(ctx, v);
    CHECK(raw != NULL, "css-font-loading: OOM reading a §2.1 descriptor argument");
    ser = ff_parse_descriptor(FF_ATTR_DESCRIPTOR[slot], raw);
    JS_FreeCString(ctx, raw);
    if (!ser) return false;
    JS_SetPropertyUint32(ctx, state, (uint32_t)slot, JS_NewString(ctx, ser));
    free(ser);
    return true;
}

/* ---- CSS FONT LOADING §2.1 "The Constructor" -----------------------------------------------------------
 *
 * "When the FontFace(family, source, descriptors) method is called, execute these steps: Let font face be a
 * fresh FontFace object. Set font face's status attribute to "unloaded", Set its internal [[FontStatusPromise]]
 * slot to a fresh pending Promise object. ... Parse the family argument, and the members of the descriptors
 * argument, ... If the source argument is a CSSOMString, parse it according to the grammar of the CSS src
 * descriptor of the @font-face rule. If any of them fail to parse correctly, ... Otherwise, set font face's
 * corresponding attributes to the serialization of the parsed values. ... Return font face. If font face's
 * status is "error", terminate this algorithm; otherwise, complete the rest of these steps asynchronously."
 * (The ellipses are CUTS and are marked as such; the numbering a reader expects beside these sentences is not
 * written for the reason the parse above gives — §2.1's algorithm is one list item holding several paragraphs,
 * so the PROSE order and the LIST order are two sequences and a number written from either cannot be checked.)
 *
 * THE SYNCHRONOUS HALF IS ALL OF IT EXCEPT THE LAST SENTENCE, AND WHAT THAT SENTENCE DEFERS IS A NAMED
 * RESIDUAL.
 *   WHAT IS NOT COVERED: everything §2.1 completes ASYNCHRONOUSLY — for a `[[Data]]` source, setting `status`
 *     to "loading", appending to every FontFaceSet's [[LoadingFonts]] list, parsing the bytes as a font, and
 *     settling into "loaded" or "error". For a `[[Urls]]` source those steps set the slot and nothing else, so
 *     a URL-sourced face's `status` of "unloaded" is what a browser reads back too until §2.2's `load()` is
 *     called — which is why this residual is about the BufferSource arm and not about both.
 *   WHAT THE NEXT DIFF BUILDS: §2.2's `load()` and the [[FontStatusPromise]] slot beside it, which is the same
 *     landing the error-arm residual above names; the data parse then has somewhere to settle.
 *   HOW ITS ABSENCE WOULD SHOW: a face constructed from an ArrayBuffer or a typed array reads `status` of
 *     "unloaded" for ever, where a browser moves it to "loading" without the page asking and then to "loaded"
 *     or "error" — so a page that polls `status` after constructing from bytes never sees it change. */
static JSValue js_ff_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue obj, state, proto;
    bool ok;
    int i;

    (void)this_val; (void)magic;
    DCHECK(g_ready, "a FontFace was constructed before its interface was declared");
    /* A DECLARED DICTIONARY POSITION IS CONVERTED EVEN WHEN THE PAGE STOPPED SHORT OF IT (core/idl_args.c
       states the rule at the count it extends), so `new FontFace(f, s)` arrives here with the object built and
       every one of §2's ten descriptor defaults already placed on it. This is the ASSERTION of that and not a
       substitute for it: a body that built its own empty object, or that read each member as "normal if
       absent", would be re-deriving the IDL's `= {}` and then each member's own default — two answers to one
       question, free to disagree the day `dictionary FontFaceDescriptors` gains an eleventh member. It is
       `>=` and not `==` because a page may pass more arguments than the IDL declares and Web IDL ignores
       them. */
    DCHECK(argc >= 3 && JS_IsObject(argv[2]),
           "§2.1's constructor reached its body without the object the dictionary conversion builds — `family` "
           "and `source` are the two required positions and `descriptors` is the one optional, which idl_args "
           "places as the all-defaults dictionary when it is omitted");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "a FontFace was constructed in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a FontFace could not be allocated");
    state = JS_NewArray(ctx);
    CHECK(!JS_IsException(state), "a FontFace's state could not be allocated");

    /* THE `status` OF "unloaded" FIRST, so the object is never observable without one — and EVERY slot is written
       before the state is placed, so `ff_state`'s reader has nothing to default past.
       THE SOURCE IS STORED AS THE ARGUMENT AND NOT AS A SERIALIZATION, which is §2.1's own two sentences: "If
       the source argument was a CSSOMString, set font face's internal [[Urls]] slot to the string" and "If the
       source argument was a BufferSource, set font face's internal [[Data]] slot to the passed argument". The
       string is PARSED below for VALIDITY, because the same section puts the `src` grammar inside the same "if
       any of them fail to parse correctly" that the descriptors are in — but what the slot holds is the string,
       which is what a §2.2 load would have to resolve its urls out of. */
    JS_SetPropertyUint32(ctx, state, FF_STATUS, JS_NewString(ctx, FF_STATUS_UNLOADED));
    JS_SetPropertyUint32(ctx, state, FF_SOURCE, JS_DupValue(ctx, argv[1]));

    /* THE PARSE. `family` is the first argument and the ten descriptors are members of the third, so the
       dictionary's members arrive here as already-converted CSSOMStrings — idl_args performed Web IDL
       §3.2.17 "Dictionary types"' walk at the CONVERSION, in the argument order Web IDL states, which is the
       whole reason the descriptors are a declared IDL_DICT rather than an object this body reads. */
    ok = ff_place_parsed(ctx, state, FF_FAMILY, argv[0]);
    /* "If the source argument is a CSSOMString, parse it according to the grammar of the CSS src descriptor of
       the @font-face rule." ONLY the string arm: a BufferSource has no grammar to be parsed by, and an unknown
       has no bytes this file may parse — both take the same non-throwing arm `ff_place_parsed` takes for its
       own reason, and the value is already in FF_SOURCE either way. The result is DISCARDED because the slot
       holds the string §2.1 says it holds; what this call is for is the VALIDITY half of the same sentence. */
    if (ok && JS_IsString(argv[1])) {
        const char *src = JS_ToCString(ctx, argv[1]);
        char *ser;

        CHECK(src != NULL, "css-font-loading: OOM reading §2.1's `source` argument");
        ser = ff_parse_descriptor("src", src);
        JS_FreeCString(ctx, src);
        ok = (ser != NULL);
        free(ser);
    }
    for (i = FF_FAMILY + 1; ok && i < FF_ATTR_COUNT; i++) {
        JSValue m = JS_GetPropertyStr(ctx, argv[2], FF_ATTR_ID[i]);

        /* AN OMITTED MEMBER IS THE DICTIONARY'S DEFAULT AND IS STILL PARSED, because §2.1 says "the
           members of the descriptors argument" and a defaulted member IS present: Web IDL §3.2.17 "Dictionary types"
           places the default value when the member is absent, so by the time a body sees the dictionary there
           is no such thing as a missing member with a default. Reading undefined here would mean idl_args
           placed neither — which is a defect in the declaration and not a case to handle. */
        if (JS_IsUndefined(m)) {
            JS_FreeValue(ctx, m);
            m = JS_NewString(ctx, FF_ATTR_DEFAULT[i]);
        }
        ok = ff_place_parsed(ctx, state, i, m);
        JS_FreeValue(ctx, m);
    }

    if (!ok) {
        /* THE ERROR ARM — "set font face's corresponding attributes to the empty string, and set font
           face's status attribute to "error"". ALL ELEVEN, and not merely the one that failed: the spec says
           "font face's corresponding attributes", which is the set the parse was about. */
        for (i = 0; i < FF_ATTR_COUNT; i++)
            JS_SetPropertyUint32(ctx, state, (uint32_t)i, JS_NewString(ctx, ""));
        JS_SetPropertyUint32(ctx, state, FF_STATUS, JS_NewString(ctx, FF_STATUS_ERROR));
    }
    JS_DefinePropertyValue(ctx, obj, g_atom, state, 0);
    return obj;                       /* §2.1's "Return font face" */
}

/* ---- CSS FONT LOADING §2 "The FontFace Interface"'s attributes -----------------------------------------
 *
 * §2 states no getter steps and no setter steps for any of the twelve, so Web IDL §3.7.6 "Attributes"' default
 * applies to all of them: the getter returns the slot and the setter stores into it. */

static JSValue js_ff_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue state = ff_state(ctx, this_val), v;

    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < FF_SLOT_COUNT,
           "a FontFace attribute was read with a magic no member of §2 declares — the magic IS the slot, so an "
           "unknown one means a name was installed without a slot to answer it");
    v = JS_GetPropertyUint32(ctx, state, (uint32_t)magic);
    JS_FreeValue(ctx, state);
    return v;
}

/* THE SETTER DOES NOT RE-PARSE, AND THAT IS §2's OWN SHAPE RATHER THAN A NARROWING. §2.1's parse is the
   CONSTRUCTOR's step; the eleven attributes are declared `attribute CSSOMString` with no setter steps of their
   own, so Web IDL §3.7.6's default store is the whole algorithm — which is why `f.family = "  a  "` reads back
   what was written in a browser and why re-parsing here would be this file inventing a step the standard does
   not have. The CONVERSION is the declaration's and has already run. */
static JSValue js_ff_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue state = ff_state(ctx, this_val);

    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < FF_ATTR_COUNT,
           "a FontFace attribute was written with a magic that is not one of §2's eleven writable attributes — "
           "`status` is readonly and the two internal slots are not attributes at all");
    DCHECK(JS_IsString(val) || concolic_is(val),
           "§2's `attribute CSSOMString` setter reached its body with something that is neither a string nor "
           "unknown external input — IDL_DOMSTRING produces the first and passes the second through, and there "
           "is no third thing for a declared CSSOMString position to be");
    JS_SetPropertyUint32(ctx, state, (uint32_t)magic, JS_DupValue(ctx, val));
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

/* ---- declaration and installation --------------------------------------------------------------------------- */

void font_face_init(JSContext *ctx)
{
    /* `constructor(CSSOMString family, (CSSOMString or BufferSource) source,
                    optional FontFaceDescriptors descriptors = {})` — two required positions, so Web IDL §3.7.1
       "Interface object"'s length is 2, which idl_step_constructor derives from this declaration. */
    static const IdlArgType CTOR_ARGS[3] = { IDL_DOMSTRING, IDL_STRING_OR_BUFFERSOURCE, IDL_DICT };
    /* `dictionary FontFaceDescriptors` — TEN members, every one a CSSOMString with a default.
       IT IS IN WEB IDL §3.2.17 Dictionary types' READ ORDER AND NOT IN `FontFaceSlot`'s, WHICH ARE TWO
       DIFFERENT ORDERS OVER ONE SET OF NAMES. This list's order is consumed by ONE thing: idl_args.c's
       member loop walks it in array order and performs §3.2.17 (ES-to-IDL list) step 4.1.3.1's `? Get` on
       the page's object in that order, which a page observes by passing an object whose members are getters.
       The standard fixes that order — step 3 is "in order from least to most derived" (the `level` column,
       and this dictionary INHERITS NOTHING so all ten are level 0) and step 4's inner loop is "in
       lexicographical order". The loop then PLACES each converted value BY NAME
       (`JS_SetPropertyStr(ctx, w->out, dm->name, ...)`), and js_ff_ctor reads it back BY NAME
       (`JS_GetPropertyStr(ctx, argv[2], FF_ATTR_ID[i])`) — so no index of this array reaches anything, and
       lexicographical order costs this file nothing.
       THE DEFAULTS ARE A SECOND COPY OF FF_ATTR_DEFAULT AND ARE ASSERTED EQUAL TO IT BELOW. They cannot be
       ONE copy: these rows are a `static const` initializer, and C does not admit `FF_ATTR_DEFAULT[i]` in
       one. The comment that used to stand here claimed "the defaults are the table above rather than a
       second copy here: the enumerator is the index into both" — which was never true of these rows, and is
       recorded rather than deleted because a reader who believes it will go looking for a sharing mechanism
       that has never existed and cannot. What makes them one FACT is the assert, not the storage. */
    static const IdlDictMember DESCRIPTORS[FF_ATTR_COUNT - 1] = {
        { "ascentOverride",    IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "normal" },
        { "descentOverride",   IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "normal" },
        { "display",           IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "auto" },
        { "featureSettings",   IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "normal" },
        { "lineGapOverride",   IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "normal" },
        { "stretch",           IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "normal" },
        { "style",             IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "normal" },
        { "unicodeRange",      IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "U+0-10FFFF" },
        { "variationSettings", IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "normal" },
        { "weight",            IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "normal" },
    };
    JSClassDef d = { "FontFace" };
    int i;

    if (g_ready) return;   /* one AGENT, one class */

    /* THE PRE-INIT VALUE OF A DECLARATION ID IS -1 AND A STATIC ARRAY'S IS 0, so the eleven are written here
       rather than at the declaration. It is a LOOP and not an initializer list because a list is a twelfth
       spelling of this enumeration's length: an initializer short by one leaves that entry at 0 silently,
       which is a real declaration id in the pool and would install SOMEBODY ELSE'S setter on this prototype.
       core/agent_state.h's release check reads -1 as the pre-init state for an id, so this is also what makes
       the FIRST agent's pre-declaration state the same as every later agent's post-release state. */
    for (i = 0; i < FF_ATTR_COUNT; i++)
        g_id_set[i] = -1;

    /* THE THREE TABLES AND THE SETTER-ID ARRAY ARE FOUR SPELLINGS OF ONE LIST, and this is the assert that
       keeps them one. `FontFaceSlot` is the index every one of them is written over — the identifier table,
       the descriptor table, the default table and the setter-id array — so an attribute added to one without
       the others would silently re-aim all four. Both operands are in scope at this line, which is what makes
       this a check rather than a restatement of a literal.
       IT USED TO SAY `AND THE DICTIONARY`, COUNTING `DESCRIPTORS` AS A FIFTH SPELLING, AND THAT WAS THE
       DEFECT RATHER THAN A LOOSE WORDING — it is recorded rather than deleted because the four names really
       are the same ten words plus `family`, so a reader will re-derive it. `DESCRIPTORS` is indexed by
       NOTHING: no consumer reads a row of it by position (idl_args.c's member loop walks it in order to fix
       the order of §3.2.17 step 4.1.3.1's `? Get`, then places BY NAME, and js_ff_ctor reads back BY NAME),
       and its order is fixed by a DIFFERENT standard's rule — §3.2.17's lexicographic one, which
       idl_dict_order_check enforces at the declaration. Calling it a fifth spelling of `FontFaceSlot` bought
       two asserts that demanded `FontFaceSlot`'s order of it, and BOTH ARE GONE: one required
       `DESCRIPTORS[FF_UNICODE_RANGE - 1]` to be `unicodeRange`, under a message whose own reason refutes it
       ("the constructor reads each member by the identifier table's name" is exactly why the order cannot
       matter); the other pinned the LAST row to `lineGapOverride` in order to catch a SHORT INITIALIZER,
       which is a real obligation and is discharged by the loop below instead. */
    DCHECK(strcmp(FF_ATTR_ID[FF_UNICODE_RANGE], "unicodeRange") == 0 &&
           strcmp(FF_ATTR_DESCRIPTOR[FF_UNICODE_RANGE], "unicode-range") == 0 &&
           strcmp(FF_ATTR_DEFAULT[FF_UNICODE_RANGE], "U+0-10FFFF") == 0,
           "the three per-attribute tables are not in the order `FontFaceSlot` declares — this is the one row "
           "whose three entries differ from each other in all three tables, so it is the pair that can be "
           "checked without re-listing the enumeration");

#if APICLIENT_DEV
    /* WHAT ACTUALLY COUPLES `DESCRIPTORS` TO `FontFaceSlot` IS A SET AND NOT AN ORDER, AND THIS IS THAT SET.
       js_ff_ctor reads the converted dictionary with `JS_GetPropertyStr(ctx, argv[2], FF_ATTR_ID[i])` for
       every attribute past `family`, so an identifier this list does not declare is one Web IDL §3.2.17
       never placed: the read answers `undefined`, the `JS_IsUndefined(m)` arm substitutes
       `FF_ATTR_DEFAULT[i]`, and a member the dictionary does not have reads back a plausible value with
       nothing anywhere to say the page's own was never looked for. That is the failure this file must not
       have, and it is a BIJECTION rather than a sequence — the two lists have equal length by the array's
       own sizing, so an injection from the identifiers into the rows is onto.
       IT IS STRICTLY STRONGER THAN THE TWO ASSERTS IT REPLACES AND NOT A WEAKENING TO FIT THE NEW ORDER.
       A SHORT INITIALIZER leaves a trailing row zeroed, whose `name` is NULL: it matches no identifier, so
       some identifier matches no row and this fires — for a hole at ANY length rather than only for a list
       short by exactly the last row, which is all the previous check could see. A MISSPELT row, a row added
       to this list and not to the enumeration, and a row whose default has drifted from `FF_ATTR_DEFAULT`
       are three more it catches and neither of the old two could.
       AND IT SAYS NOTHING ABOUT ROW ORDER, which is the point: §3.2.17's order is idl_dict_order_check's to
       enforce, at the declaration, over every dictionary in the engine — a second statement of it here would
       be this file's own answer to a question the machine already asks. */
    for (i = FF_FAMILY + 1; i < FF_ATTR_COUNT; i++) {
        int at = -1, j;

        for (j = 0; j < FF_ATTR_COUNT - 1; j++)
            if (DESCRIPTORS[j].name != NULL && strcmp(DESCRIPTORS[j].name, FF_ATTR_ID[i]) == 0) {
                DCHECKF(at < 0,
                        "`dictionary FontFaceDescriptors` declares §2's `%s` TWICE — two rows of one "
                        "identifier are two §3.2.17 step 4.1.3.1 `Get`s of one property, and because the "
                        "second placement wins, the row a reader edits is not necessarily the one that "
                        "answers", FF_ATTR_ID[i]);
                at = j;
            }
        DCHECKF(at >= 0,
                "§2's writable attribute `%s` is not a declared member of `dictionary "
                "FontFaceDescriptors` — either a row of the initializer is missing or zeroed (a short list "
                "leaves `name` NULL), or one of the two lists gained a name the other did not. The "
                "constructor reads this identifier off the converted dictionary, so an undeclared one reads "
                "`undefined` and is silently answered by this file's own default instead of by the page's "
                "value", FF_ATTR_ID[i]);
        DCHECKF(at < 0 || (DESCRIPTORS[at].dflt == IDL_DEFAULT_STRING && DESCRIPTORS[at].dflt_str != NULL &&
                           strcmp(DESCRIPTORS[at].dflt_str, FF_ATTR_DEFAULT[i]) == 0),
                "§2's `%s` has one default in `dictionary FontFaceDescriptors` and another in "
                "FF_ATTR_DEFAULT — §3.2.17 step 4.1.5 places the FORMER when the page omits the member, and "
                "js_ff_ctor's omitted-member arm substitutes the LATTER, so the two disagreeing means one "
                "value is read back by a page that passed no descriptors and a different one by a page that "
                "passed `undefined` for it", FF_ATTR_ID[i]);
    }
#endif

    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);

    g_key = JS_NewSymbol(ctx, "fontFaceState", false);
    CHECK(!JS_IsException(g_key), "the FontFace state slot key allocation failed");
    g_atom = JS_ValueToAtom(ctx, g_key);
    CHECK(g_atom != JS_ATOM_NULL, "the FontFace state slot key could not be interned");

    g_id_ctor = idl_method_id_dict(ctx, CTOR_ARGS, 3, DESCRIPTORS,
                                   (int)(sizeof(DESCRIPTORS) / sizeof(DESCRIPTORS[0])), js_ff_ctor, 0);
    idl_optional_from(2);                /* `optional FontFaceDescriptors descriptors = {}` */
    for (i = 0; i < FF_ATTR_COUNT; i++)
        g_id_set[i] = idl_setter_id(ctx, IDL_DOMSTRING, false, js_ff_set, i);

    DCHECK(g_id_ctor >= 0, "§2.1's constructor declaration did not enter the argument pool");
    for (i = 0; i < FF_ATTR_COUNT; i++)
        DCHECKF(g_id_set[i] >= 0,
                "the setter declaration for §2's `%s` did not enter the argument pool — the prototype install "
                "reads this array by the same index, and a -1 there would install the attribute READONLY with "
                "nothing to say so", FF_ATTR_ID[i]);

    realm_declare_intrinsic(font_face_install_proto);
    g_ready = 1;

    agent_state_flag("font_face", &g_ready, "§2's FontFace declaration latch");
    agent_state_class("font_face", &g_class, "§2's FontFace class");
    agent_state_value("font_face", &g_key, "the face's state-slot key");
    agent_state_atom("font_face", &g_atom, "the face's state-slot key, interned");
    agent_state_id("font_face", &g_id_ctor, "§2.1's constructor declaration");
    for (i = 0; i < FF_ATTR_COUNT; i++)
        agent_state_id("font_face", &g_id_set[i], "one of §2's eleven attribute setter declarations");
}

void font_face_install_proto(JSContext *ctx)
{
    JSValue proto, prev;
    int i;

    DCHECK(g_class != 0, "a realm asked for FontFace.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "font_face_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "FontFace.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "FontFace");
    for (i = 0; i < FF_ATTR_COUNT; i++)
        idl_install_accessor(ctx, proto, FF_ATTR_ID[i], js_ff_get, i, g_id_set[i]);
    /* `readonly attribute FontFaceLoadStatus status` — no setter id, which is what makes it readonly. */
    idl_install_accessor(ctx, proto, "status", js_ff_get, FF_STATUS, -1);
    JS_SetClassProto(ctx, g_class, proto);
}

void font_face_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    DCHECK(g_id_ctor >= 0, "FontFace was installed before font_face_init declared it");
    ctor = idl_step_constructor(ctx, "FontFace", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the FontFace interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "FontFace was installed in a realm that never ran its prototype install");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    idl_define_global_property_reference(ctx, global, "FontFace", ctor);
}

/* THIS CLASS HAS NO FINALIZER AND NO gc_mark, which is why zeroing its id below reaches nothing — see
 * font_face.h for the whole of that argument. */
void font_face_free(JSRuntime *rt)
{
    int i;

    /* NOT `if (!g_ready) return;` — this is reached from a release column whose declare pass is
       unconditional, so a release in an agent that never declared is the thing to CRASH on. */
    DCHECK(g_ready, "§2's FontFace was released in an agent that never declared it");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom);
    g_atom = JS_ATOM_NULL;
    g_class = 0;
    g_id_ctor = -1;
    for (i = 0; i < FF_ATTR_COUNT; i++)
        g_id_set[i] = -1;
    g_ready = 0;
}
