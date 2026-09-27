/* CSS Fonts Module Level 4's `@font-feature-values` membership table. See css_font_feature_values.h for why
 * the list is closed, why it is a component of its own, and why a name in it is DROPPED outside the enclosing
 * rule rather than crashed on. Every name below is copied from CSS Fonts 4 §6.9.1 "Basic syntax"'s own
 * `<font-feature-value-type>` production, in that production's order, so the two can be read side by side. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_font_feature_values.h"

/* CSS Fonts 4 §6.9.1's `<font-feature-value-type>` alternatives, in the spec's own order — the seven feature
   value blocks it defines, each `@<name> { <declaration-list> }`.
   THE ORDER IS THE PRODUCTION'S AND NOT ASCII, deliberately: this table is READ against the spec's one line and
   a lookup over seven names does not need a sort. Where a table's order is load-bearing for the lookup (the
   recognized-at-rule registry, which binary-searches) the order is asserted at the lookup; here it is not, so
   asserting an order the spec does not write would be a second fact able to disagree with the source it was
   transcribed from. */
static const char *const FEATURE_VALUE_AT_RULES[] = {
    "stylistic", "historical-forms", "styleset", "character-variant",
    "swash", "ornaments", "annotation",
};

bool css_font_feature_value_at_rule(const char *name)
{
    const size_t n = sizeof FEATURE_VALUE_AT_RULES / sizeof FEATURE_VALUE_AT_RULES[0];
    size_t i;

    DCHECK(name != NULL,
           "an at-rule was asked whether it is one of CSS Fonts 4 §6.9.1 \"Basic syntax\"'s feature value "
           "blocks without giving its name. The name is the only thing that can decide it — CSS Fonts 4 "
           "§6.9.1's production is a list of at-keywords and nothing else — so a null here is a caller that "
           "lost the name rather than an at-rule that never had one");
    for (i = 0; i < n; i++)
        if (strcmp(FEATURE_VALUE_AT_RULES[i], name) == 0) return true;
    return false;
}

/* ============================================================================================================
 * css-fonts-4 §12.2 "The CSSFontFeatureValuesRule interface"'s `CSSFontFeatureValuesMap`, and css-fonts-4
 * §6.9.1 "Basic syntax"'s per-block value grammar it is filled from.
 *
 * IT IS A `maplike<CSSOMString, sequence<unsigned long>>` AND THEREFORE Web IDL §3.7.11 "Maplike declarations"
 * AND NOT §3.7.9 "Iterable declarations", which is the one thing about this interface that is easy to get
 * wrong and observable when it is. The two sections put the same five members on a prototype — `entries`,
 * `keys`, `values`, `forEach` and @@iterator — and disagree about the ITERATOR: §3.7.9.2 "Iterator prototype
 * object" gives it %IteratorPrototype% and a tag of the interface name plus " Iterator", while §3.7.11.2
 * "%Symbol.iterator%" builds it with
 * `CreateIteratorFromClosure(closure, "%MapIteratorPrototype%", %MapIteratorPrototype%)`, whose own tag is
 * `Map Iterator`. core/idl_iter.h's `maplike` flag is what states that, and the SIX members §3.7.11 adds —
 * §3.7.11.1 `size`, §3.7.11.7 `get`, §3.7.11.8 `has`, §3.7.11.9 `set`, §3.7.11.10 `delete`, §3.7.11.11 `clear`
 * — are here, because their answers are the map entries' own contents rather than a walk over an index.
 *
 * §12.2 DECLARES ITS OWN `set` AND THAT IS WHY IT IS WRITTEN OUT. §3.7.11.9's default exists only "if A does
 * not declare a member with identifier set", and §12.2 does: `undefined set(CSSOMString featureValueName,
 * (unsigned long or sequence<unsigned long>) values)`. Two things follow. Its value argument is a UNION, whose
 * numeric arm §12.2 explains — "a single unsigned long value is treated as a sequence of a single value" — and
 * whose conversion is the declared type's rather than this body's (core/idl_args.h's
 * IDL_UNSIGNED_LONG_OR_SEQUENCE). And its RETURN is `undefined` where §3.7.11.9's trailing sentence says a
 * declared `set` "must return this": the two standards disagree, the IDL's declared return type is what the
 * binding converts to, and `undefined` is what a browser answers — so the declaration wins and the
 * disagreement is recorded here rather than resolved silently.
 * ============================================================================================================ */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "quickjs.h"
#include "core/agent_state.h"
#include "core/css/css_font_feature_values.h"
#include "core/idl_args.h"
#include "core/idl_iter.h"
#include "core/realm.h"
#include "solver/concolic.h"   /* §12.2's `set` stores a CROSSED feature index as itself — see its body */

static JSClassID g_map_class;
static JSValue   g_entries_key = JS_UNDEFINED;
static JSValue   g_kind_key = JS_UNDEFINED;
static JSAtom    g_atom_entries = JS_ATOM_NULL;
static JSAtom    g_atom_kind = JS_ATOM_NULL;
static int       g_pair_handle = -1;
static int       g_id_get = -1, g_id_has = -1, g_id_set = -1, g_id_delete = -1, g_id_clear = -1;

/* css-fonts-4 §6.9.1's per-block COUNT, in the production's own order. See the header for why 0 is UNBOUNDED
   and a real answer, and for why §6.9.1's RANGE halves are not asked here. */
unsigned css_font_feature_values_max_values(const char *name)
{
    /* "Each declaration's value in @annotation, @ornaments, @stylistic, @swash, must match the grammar
       <font-feature-index>" — ONE, which css-fonts-4 §6.9.2's own example marks as such
       (`@swash { swishy: 3 5; }` is annotated "more than 1 value for swash: syntax error"). */
    if (strcmp(name, "stylistic") == 0 || strcmp(name, "swash") == 0 ||
        strcmp(name, "ornaments") == 0 || strcmp(name, "annotation") == 0)
        return 1;
    /* "Each declaration's value in @character-variant must match the grammar
       <font-feature-index [0,99]> <font-feature-index [0,∞]>" — TWO, and the count is css-fonts-4 §6.9.2's
       sentence rather than that production's arity: "If more than two values are assigned to a given name, a
       syntax error occurs and the entire feature value definition is ignored." The production as written
       requires two where §6.9.2's own examples accept one (`@character-variant { gamma: 12; }` is annotated
       "implies cv12 1"), so the two halves of css-fonts-4 disagree about the MINIMUM and agree about the
       maximum; §6.9.2 is the section that states what the numbers mean and is followed. */
    if (strcmp(name, "character-variant") == 0) return 2;
    /* `@styleset` is `<font-feature-index [0,20]>+` — a `+`, so no maximum; and `@historical-forms` has no
       per-declaration grammar in §6.9.1 at all, so there is no count to state and inventing one would be
       inventing. Both are therefore UNBOUNDED, and the two reasons are different. */
    DCHECK(strcmp(name, "styleset") == 0 || strcmp(name, "historical-forms") == 0,
           "css-fonts-4 §6.9.1's per-block value count was asked about an at-keyword that is not one of that "
           "section's seven `<font-feature-value-type>` alternatives. The count is a property of WHICH block "
           "this is, so a name outside the production has none — css_font_feature_value_at_rule is the "
           "membership question and is asked first");
    return 0;
}

/* ---- the map entries ------------------------------------------------------------------------------------- */

/* The entries Array. An own SLOT read and never a property lookup, for core/css/media_list.c's reason: a lookup
   walks the prototype chain into the solver's absent-state seam and would mint a concolic for a name nobody
   defined. JS_UNDEFINED for a stranger, which is what the brand below tests. */
static JSValue ffv_entries(JSContext *ctx, JSValueConst v)
{
    JSValue arr;

    if (!JS_IsObject(v)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &arr, v, g_atom_entries) <= 0) return JS_UNDEFINED;
    return arr;
}

bool css_font_feature_values_map_is(JSContext *ctx, JSValueConst v)
{
    JSValue arr = ffv_entries(ctx, v);
    bool ok = JS_IsArray(arr);

    JS_FreeValue(ctx, arr);
    return ok;
}

static uint32_t ffv_count(JSContext *ctx, JSValueConst map)
{
    JSValue arr = ffv_entries(ctx, map), len;
    uint32_t n = 0;

    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return 0; }
    len = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_FreeValue(ctx, arr);
    return n;
}

/* One entry's half — 0 is the name, 1 is the values Array. OWNED. */
static JSValue ffv_entry_at(JSContext *ctx, JSValueConst map, uint32_t i, uint32_t half)
{
    JSValue arr = ffv_entries(ctx, map), pair, out;

    DCHECK(JS_IsArray(arr), "a CSSFontFeatureValuesMap entry was read off an object that holds no entries");
    pair = JS_GetPropertyUint32(ctx, arr, i);
    JS_FreeValue(ctx, arr);
    DCHECK(JS_IsArray(pair),
           "a CSSFontFeatureValuesMap's entry list holds something that is not a `[name, values]` pair — every "
           "site that appends one appends both halves together, so a third shape means a writer bypassed them");
    out = JS_GetPropertyUint32(ctx, pair, half);
    JS_FreeValue(ctx, pair);
    return out;
}

/* WHICH ENTRY A KEY NAMES, or -1. Web IDL §2.5.11 "Maplike declarations" keys map entries on the SAME
   equality a JS Map does, so the comparison is byte-for-byte over the key string — which is also css-fonts-4
   §6.9.1's own requirement that feature value names "are case-sensitive (so foo: 1; and FOO: 2 define two
   different features)". */
static int ffv_index_of(JSContext *ctx, JSValueConst map, const char *key)
{
    uint32_t n = ffv_count(ctx, map), i;

    for (i = 0; i < n; i++) {
        JSValue name = ffv_entry_at(ctx, map, i, 0);
        const char *c = JS_ToCString(ctx, name);
        bool hit = c && strcmp(c, key) == 0;

        if (c) JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, name);
        if (hit) return (int)i;
    }
    return -1;
}

/* §12.2's stored value AS A JS VALUE — a NEW Array every time, which is the sequence type's own answer and not
   a defensive choice made here: handing back the stored Array would let a page write into the rule's own
   record, so `m.get("di").push(9)` would change what the next `get` answers and what `cssText` prints.
   WEB IDL SAYS IT AS A PROPERTY OF THE TYPE RATHER THAN OF THIS MEMBER. Web IDL §2.13.28 "Sequence types —
   sequence< T >": "any sequence returned from a platform object will be a copy and modifications made to it
   will not be visible to the platform object". Its binding half, Web IDL §3.2.21 "Sequences — sequence< T >",
   is that sentence as steps — "Let A be a new Array object created as if by the expression" — so the copy IS
   the conversion. OWNED. */
static JSValue ffv_values_copy(JSContext *ctx, JSValueConst stored)
{
    JSValue out = JS_NewArray(ctx), len;
    uint32_t n = 0, i;

    CHECK(!JS_IsException(out), "cssom: a CSSFontFeatureValuesMap value list could not be allocated");
    len = JS_GetPropertyStr(ctx, stored, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, i, JS_GetPropertyUint32(ctx, stored, i));
    return out;
}

/* Store `values` (an Array, CONSUMED) under `key`, replacing an entry of that name — css-fonts-4 §6.9.1's
   "If the same tuple appears more than once in a document … the last-defined one is used", which is also
   Web IDL §3.7.11.9's "Set map[key] to value". A replacement keeps the entry's POSITION, exactly as a JS Map's
   `set` does, so §2.5.11's insertion order is not disturbed by an overwrite. */
static void ffv_put(JSContext *ctx, JSValueConst map, const char *key, JSValue values)
{
    JSValue arr = ffv_entries(ctx, map), pair;
    int at = ffv_index_of(ctx, map, key);

    DCHECK(JS_IsArray(arr), "a CSSFontFeatureValuesMap was written on an object that holds no entries");
    DCHECK(JS_IsArray(values), "a CSSFontFeatureValuesMap entry was given a value that is not a list — §12.2's "
                               "value type is `sequence<unsigned long>` and the get() method \"always returns a "
                               "sequence of values, even if the sequence only contains a single value\"");
    if (at >= 0) {
        pair = JS_GetPropertyUint32(ctx, arr, (uint32_t)at);
        JS_SetPropertyUint32(ctx, pair, 1, values);
        JS_FreeValue(ctx, pair);
        JS_FreeValue(ctx, arr);
        return;
    }
    pair = JS_NewArray(ctx);
    CHECK(!JS_IsException(pair), "cssom: a CSSFontFeatureValuesMap entry could not be allocated");
    JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, key));
    JS_SetPropertyUint32(ctx, pair, 1, values);
    JS_SetPropertyUint32(ctx, arr, ffv_count(ctx, map), pair);
    JS_FreeValue(ctx, arr);
}

/* Which of the seven this map is — the at-keyword with no `@`, out of the own slot the mint filled. BORROWED
   through a C string the caller frees. */
static char *ffv_kind(JSContext *ctx, JSValueConst map)
{
    JSValue v;
    const char *c;
    char *out;

    if (JS_GetOwnSlot(ctx, &v, map, g_atom_kind) <= 0) return NULL;
    c = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!c) return NULL;
    out = strdup(c);
    JS_FreeCString(ctx, c);
    CHECK(out != NULL, "cssom: OOM reading a CSSFontFeatureValuesMap's block kind");
    return out;
}

/* ---- css-fonts-4 §6.9.1's FEATURE VALUE DECLARATIONS ------------------------------------------------------ */

/* CSS Syntax 3 §4.2 "Definitions"' WHITESPACE — newline, tab and space. A serialized block joins with single
   spaces, and the value half is the page's own span, so every one of them can occur in it. */
static bool ffv_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* CSS Syntax 3 §4.3.1 "Consume a token"'s first step over a value span, plus §4.2 "Definitions"' whitespace.
   A COMMENT IS CONSUMED BY THE TOKENIZER AND IS THEREFORE NOT PART OF THE VALUE, so a feature value written
   with a comment between two of its indexes is a TWO-value declaration and refusing it would be a divergence
   rather than a narrowing. The spans this walk reads are the page's own (see CSSOM_BLOCK_FEATURE_VALUES), which
   is why a comment can be in one at all. An unterminated comment runs to the end of the value, which is
   §4.3.2 "Consume comments"' own arm. */
static void ffv_skip_ws(const char **p, const char *end)
{
    while (*p < end) {
        if (ffv_is_ws(**p)) { (*p)++; continue; }
        if (end - *p >= 2 && (*p)[0] == '/' && (*p)[1] == '*') {
            *p += 2;
            while (*p < end && !(end - *p >= 2 && (*p)[0] == '*' && (*p)[1] == '/')) (*p)++;
            if (end - *p >= 2) *p += 2; else *p = end;
            continue;
        }
        return;
    }
}

/* ONE `<font-feature-index>` — css-fonts-4 §6.9.1's "the value must be a list of one or more NON-NEGATIVE
   <integer>s", so a `-` is outside the grammar (§6.9.1's own example annotates `flowing: -1` as a "negative
   value" and the rule it sits in drops that declaration). CSS Values 4 §5.2 "Integers: the <integer> type"
   admits a leading `+`, and non-negative admits it too, so it is consumed.
   THE UPPER END IS CLAMPED AND NOT REFUSED, which is CSS Values 4 §5 "Numeric Data Types"' own rule rather
   than a choice made here: "when a value cannot be explicitly supported due to range/precision limitations, it
   must be converted to the closest value supported by the implementation". §12.2's map value type is
   `sequence<unsigned long>`, so the supported range IS 32 bits unsigned and the closest supported value for
   anything above it is UINT32_MAX. Refusing instead would make a declaration invalid where css-fonts-4 §6.9.2
   says of an out-of-range feature index that "values greater than 99 or equal to 0 do not generate a syntax
   error when parsed but enable no OpenType features".
   Answers false when there is no digit at `*p`, which is the grammar failing. */
static bool ffv_take_index(const char **p, const char *end, uint32_t *out)
{
    uint64_t v = 0;
    bool any = false;

    if (*p < end && **p == '+') (*p)++;
    while (*p < end && **p >= '0' && **p <= '9') {
        any = true;
        if (v <= 0xFFFFFFFFULL) v = v * 10u + (uint64_t)(**p - '0');
        (*p)++;
    }
    if (!any) return false;
    *out = v > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)v;
    return true;
}

/* ONE feature value declaration's VALUE — `<font-feature-index>+` with `max` as css-fonts-4 §6.9.1's count.
   Returns the values as an Array, or JS_UNDEFINED for a value outside the grammar, which is §6.9.1's "the
   declaration is invalid and must be ignored". OWNED. */
static JSValue ffv_parse_values(JSContext *ctx, const char *text, size_t len, unsigned max)
{
    const char *p = text, *end = text + len;
    JSValue out = JS_NewArray(ctx);
    uint32_t n = 0, v;

    CHECK(!JS_IsException(out), "cssom: a feature value list could not be allocated");
    for (;;) {
        ffv_skip_ws(&p, end);
        if (p == end) break;
        if (!ffv_take_index(&p, end, &v)) { JS_FreeValue(ctx, out); return JS_UNDEFINED; }
        /* §6.9.1's count, checked as the value is read rather than afterwards, so a block declaring a hundred
           indexes for a `@swash` costs one comparison instead of a hundred stores. 0 is unbounded. */
        if (max && n >= max) { JS_FreeValue(ctx, out); return JS_UNDEFINED; }
        JS_SetPropertyUint32(ctx, out, n++, JS_NewUint32(ctx, v));
    }
    /* `+` IS "ONE OR MORE", so an empty value is not a `<font-feature-index>+`. `@swash { a: }` reaches here
       with nothing, and a value holding only a comment does too once the comment is consumed. */
    if (n == 0) { JS_FreeValue(ctx, out); return JS_UNDEFINED; }
    return out;
}

/* The serialized block, split at the top level. CSSOM §6.6's serialize a CSS declaration block produces
   `name: value;` per declaration joined by a single space, and the values here are `<font-feature-index>` lists
   holding no strings, no functions and no braces, so a plain scan for `;` and `:` is the whole of what
   separating them takes. A comment CAN occur inside a value — the spans are the page's own — which is why the
   value half is handed to a walk that consumes one rather than being trimmed here, and an `!important` a page
   wrote reaches that walk as a token `<font-feature-index>+` does not admit, so the declaration is dropped,
   which is §6.9.1's own answer for a value outside its grammar.
 *
 * NAMED RESIDUAL — A FEATURE VALUE NAME WRITTEN WITH A CSS ESCAPE IS NOT THE IDENT IT SPELLS.
 *   NOT COVERED: css-fonts-4 §6.9.1 makes a feature value name "any css identifier", and CSS Syntax 3 §4.3.11
 *     "Consume an ident sequence" lets one carry escapes — so `a\;b` and `a\3A b` are the single idents `a;b`
 *     and `a:b`. What is stored here is the SOURCE SPAN (see CSSOM_BLOCK_FEATURE_VALUES's own reason: the
 *     registry's canonicalisation would fold the case §6.9.1 makes significant), so such a name is stored with
 *     its backslashes in it, and the scan below additionally splits the declaration at the escaped `;` or `:`
 *     as though it were the separator. Both halves are the one gap: the span is not the ident's VALUE.
 *   WHAT THE NEXT DIFF BUILDS: CSS Syntax 3 §4.3.11's ident-sequence consumption applied to the name span, so
 *     the stored key is the ident's value; core/css/css_font_family.c already carries that walk for
 *     `<font-family-name>`'s `<custom-ident>+` arm (ff_consume_ident) and it is the entry to route to rather
 *     than a second copy of the same production. With the name a VALUE rather than a span, the split below is
 *     over tokens rather than over bytes and the second half goes with it.
 *   HOW ITS ABSENCE WOULD SHOW: a `@styleset` declaring an escaped name has one fewer entry in `size` than the
 *     block has declarations, and `get` of the unescaped ident answers undefined while `keys()` yields a string
 *     with a backslash in it. */
void css_font_feature_values_map_fill(JSContext *ctx, JSValueConst map, const char *block)
{
    const char *p, *end;
    char *kind;
    unsigned max;

    DCHECK(css_font_feature_values_map_is(ctx, map),
           "css-fonts-4 §6.9.1's feature value declarations were filled into something that is not a "
           "CSSFontFeatureValuesMap");
    if (!block || !*block) return;
    kind = ffv_kind(ctx, map);
    DCHECK(kind != NULL,
           "a CSSFontFeatureValuesMap was filled without knowing WHICH of css-fonts-4 §6.9.1's seven blocks it "
           "is. The count limit is a property of the block, so the mint stores the at-keyword and a map with "
           "none cannot judge a declaration at all");
    if (!kind) return;
    max = css_font_feature_values_max_values(kind);
    free(kind);
    p = block;
    end = block + strlen(block);
    while (p < end) {
        const char *semi = p, *colon = NULL, *name_end;
        char *name;
        JSValue values;

        while (semi < end && *semi != ';') {
            if (*semi == ':' && !colon) colon = semi;
            semi++;
        }
        if (!colon) { p = semi < end ? semi + 1 : end; continue; }   /* no `:` — not a declaration at all */
        name_end = colon;
        while (name_end > p && ffv_is_ws(name_end[-1])) name_end--;
        while (p < name_end && ffv_is_ws(*p)) p++;
        if (p == name_end) { p = semi < end ? semi + 1 : end; continue; }   /* an empty name is no `<ident>` */
        values = ffv_parse_values(ctx, colon + 1, (size_t)(semi - colon - 1), max);
        if (!JS_IsUndefined(values)) {
            name = malloc((size_t)(name_end - p) + 1);
            CHECK(name != NULL, "cssom: OOM copying a feature value name");
            memcpy(name, p, (size_t)(name_end - p));
            name[name_end - p] = '\0';
            ffv_put(ctx, map, name, values);
            free(name);
        }
        /* §6.9.1: "A syntax error within a font feature value declaration makes the declaration invalid and
           ignored, but does not invalidate the font feature value block it occurs in." So a refused value
           simply advances to the next declaration. */
        p = semi < end ? semi + 1 : end;
    }
}

char *css_font_feature_values_map_serialize(JSContext *ctx, JSValueConst map)
{
    uint32_t n = ffv_count(ctx, map), i;
    char *kind, *out;
    size_t cap, at;

    if (n == 0) return NULL;
    kind = ffv_kind(ctx, map);
    DCHECK(kind != NULL, "a CSSFontFeatureValuesMap was serialized without knowing which block it is");
    if (!kind) return NULL;
    cap = strlen(kind) + 8;
    out = malloc(cap);
    CHECK(out != NULL, "cssom: OOM serializing a feature value block");
    at = (size_t)snprintf(out, cap, "@%s {", kind);
    free(kind);
    for (i = 0; i < n; i++) {
        JSValue name = ffv_entry_at(ctx, map, i, 0);
        JSValue vals = ffv_entry_at(ctx, map, i, 1);
        const char *c = JS_ToCString(ctx, name);
        JSValue len;
        uint32_t m = 0, j;
        char *grown;

        JS_FreeValue(ctx, name);
        if (!c) { JS_FreeValue(ctx, vals); free(out); return NULL; }
        len = JS_GetPropertyStr(ctx, vals, "length");
        JS_ToUint32(ctx, &m, len);
        JS_FreeValue(ctx, len);
        cap = at + strlen(c) + 4 + (size_t)m * 12 + 4;
        grown = realloc(out, cap);
        CHECK(grown != NULL, "cssom: OOM serializing a feature value block");
        out = grown;
        at += (size_t)snprintf(out + at, cap - at, " %s:", c);
        JS_FreeCString(ctx, c);
        for (j = 0; j < m; j++) {
            JSValue v = JS_GetPropertyUint32(ctx, vals, j);
            uint32_t u = 0;

            JS_ToUint32(ctx, &u, v);
            JS_FreeValue(ctx, v);
            at += (size_t)snprintf(out + at, cap - at, " %u", u);
        }
        at += (size_t)snprintf(out + at, cap - at, ";");
        JS_FreeValue(ctx, vals);
    }
    {
        char *grown = realloc(out, at + 3);

        CHECK(grown != NULL, "cssom: OOM closing a feature value block");
        out = grown;
        memcpy(out + at, " }", 3);
    }
    return out;
}

/* ---- Web IDL §3.7.11's members ---------------------------------------------------------------------------- */

/* The receiver, brand-checked. Every member is on the PROTOTYPE, so a page can apply one to anything at all and
   the answer is a TypeError — §3.7.11's own steps, each of which begins "Let O be the this value, implementation
   checked against A". */
static bool ffv_here(JSContext *ctx, JSValueConst v, const char *member)
{
    if (css_font_feature_values_map_is(ctx, v)) return true;
    JS_ThrowTypeError(ctx, "CSSFontFeatureValuesMap.prototype.%s was reached on something that is not a "
                           "CSSFontFeatureValuesMap", member);
    return false;
}

/* §3.7.11.1 `size`: "Let map be the map entries … Return map's size, converted to a JavaScript value." It is an
   ACCESSOR and not a data property, which is that section's own `[[Get]]: G` characteristic. */
static JSValue js_ffv_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!ffv_here(ctx, this_val, "size")) return JS_EXCEPTION;
    return JS_NewUint32(ctx, ffv_count(ctx, this_val));
}

enum { FFV_GET = 0, FFV_HAS, FFV_SET, FFV_DELETE, FFV_CLEAR };

static JSValue js_ffv_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const char *const NAMES[] = { "get", "has", "set", "delete", "clear" };
    const char *key = NULL;
    int at;
    JSValue out = JS_UNDEFINED;

    if (!ffv_here(ctx, this_val, NAMES[magic])) return JS_EXCEPTION;
    /* §3.7.11.11 `clear`: "Clear map." The NOTE beside it is the invariant this must not break — "the map is
       preserved because there may be existing iterators currently suspended iterating over it" — so the Array
       is TRUNCATED IN PLACE rather than replaced, which is also what keeps every mutation a property write the
       per-flow COW delta captures. §12.2 declares no `clear` of its own, so this IS §3.7.11.11's default. */
    if (magic == FFV_CLEAR) {
        JSValue arr = ffv_entries(ctx, this_val);

        DCHECK(argc == 0, "§3.7.11.11's `clear` takes no argument and reached its body with one");
        JS_SetPropertyStr(ctx, arr, "length", JS_NewUint32(ctx, 0));
        JS_FreeValue(ctx, arr);
        return JS_UNDEFINED;
    }
    DCHECK(argc >= 1, "a CSSFontFeatureValuesMap member that takes a key reached its body with none — the key "
                      "is a required argument of every one of them, so §3.6's argument-count check refuses a "
                      "shorter call first");
    /* A REAL STRING BY NOW: the declaration converted it. §3.7.11.7/.8/.10 each say "Let key be keyArg
       converted to an IDL value of type keyType", and keyType is §12.2's `CSSOMString`. */
    key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;
    at = ffv_index_of(ctx, this_val, key);
    switch (magic) {
    /* §3.7.11.7 `get`: "If map[key] exists, then return map[key], converted to a JavaScript value. Return
       undefined." §12.2's own note is what the copy below is about: "The get() method always returns a
       sequence of values, even if the sequence only contains a single value." */
    case FFV_GET:
        if (at >= 0) {
            JSValue stored = ffv_entry_at(ctx, this_val, (uint32_t)at, 1);

            out = ffv_values_copy(ctx, stored);
            JS_FreeValue(ctx, stored);
        }
        break;
    /* §3.7.11.8 `has`: "If map[key] exists, then return true. Otherwise, return false." */
    case FFV_HAS:
        out = JS_NewBool(ctx, at >= 0);
        break;
    /* §3.7.11.10 `delete`: "Let retVal be true if map[key] exists, or else false. Remove map[key]. Return
       retVal." §12.2 declares no `delete` of its own, so this IS that default — and Web IDL §3.7.11.10's
       trailing sentence about a DECLARED one, which asks it to "return a boolean indicating whether the key was
       present or not", therefore does not arise. */
    case FFV_DELETE: {
        JSValue arr = ffv_entries(ctx, this_val);
        uint32_t n = ffv_count(ctx, this_val), i;

        out = JS_NewBool(ctx, at >= 0);
        if (at >= 0) {
            for (i = (uint32_t)at + 1; i < n; i++)
                JS_SetPropertyUint32(ctx, arr, i - 1, JS_GetPropertyUint32(ctx, arr, i));
            JS_SetPropertyStr(ctx, arr, "length", JS_NewUint32(ctx, n - 1));
        }
        JS_FreeValue(ctx, arr);
        break;
    }
    /* §12.2's OWN `set`, whose steps that section states in prose: "It takes a sequence of unsigned integers
       and associates it with a given featureValueName. The method behaves the same as the default map class
       method except that a single unsigned long value is treated as a sequence of a single value. The method
       throws an exception if an invalid number of values is passed in. If the associated feature value block
       only allows a limited number of values, the set() method throws an InvalidAccessError exception when the
       input sequence to set() contains more than the limited number of values."
       THE UNION HAS ALREADY RESOLVED, so what arrives is either an Array (§3.2.21's sequence conversion, which
       is where "treated as a sequence of a single value" becomes a one-element list) or a Number — the
       declaration's IDL_UNSIGNED_LONG_OR_SEQUENCE, never this body's test of the page's value.
       THE THROW IS ABOUT THE COUNT AND NOT THE RANGE. §6.9.1's `[0,20]` on `@styleset` is contradicted by
       §6.9.2 in the same document, and `css/cssom/CSSFontFeatureValuesRule.html` asserts `set("di", 43)`
       stores 43 — so a range check here would fail the corpus and would be inventing a refusal the standard
       withdraws two sections later. */
    default: {
        JSValue values;
        uint32_t n = 1;
        char *kind;
        unsigned max;

        DCHECK(magic == FFV_SET, "a CSSFontFeatureValuesMap member ran with a magic §12.2 does not declare");
        DCHECK(argc >= 2, "§12.2's `set` reached its body without its `values` argument — both of its IDL "
                          "arguments are required");
        if (JS_IsArray(argv[1])) {
            JSValue len = JS_GetPropertyStr(ctx, argv[1], "length");

            JS_ToUint32(ctx, &n, len);
            JS_FreeValue(ctx, len);
        } else {
            /* §12.2's NUMERIC ARM — "a single unsigned long value is treated as a sequence of a single value",
               so the count is 1 and the value goes in as itself.
               UNKNOWN EXTERNAL INPUT IS THE SAME ARM AND IS NOT AN ERROR HERE, which is why there is no
               refusal and no fork. `IDL_UNSIGNED_LONG_OR_SEQUENCE` CROSSES an unknown (core/idl_args.h says
               why, and why declaring a fork would be one union of that shape answering differently from its
               sibling), so a `map.set(k, location.hash.length)` arrives holding the unknown — and a feature
               index is DATA this engine never branches on, so storing it is `a sequence of a single value` for
               exactly the same reason a known number is and the opacity survives the boundary. What the
               declaration DOES guarantee is that the value is one of the two, which is this codebase's own
               invariant and not a claim about the page's bytes. */
            DCHECK(JS_IsNumber(argv[1]) || concolic_is(argv[1]),
                   "css-fonts-4 §12.2's `set` reached its body with a `values` argument that is neither a "
                   "sequence, a number, nor unknown external input. Web IDL §3.2.25 \"Union types\" over "
                   "`(unsigned long or sequence<unsigned long>)` has exactly two arms and §3.2.6 "
                   "\"unsigned long\" refuses nothing, so a third shape means the declaration does not carry "
                   "IDL_UNSIGNED_LONG_OR_SEQUENCE at this position");
        }
        kind = ffv_kind(ctx, this_val);
        DCHECK(kind != NULL, "a CSSFontFeatureValuesMap was `set` without knowing which block it is");
        max = kind ? css_font_feature_values_max_values(kind) : 0u;
        free(kind);
        if (max && n > max) {
            JS_FreeCString(ctx, key);
            return JS_ThrowDOMException(ctx, "InvalidAccessError",
                                        "this feature value block admits at most %u value%s per name",
                                        max, max == 1 ? "" : "s");
        }
        values = JS_IsArray(argv[1]) ? ffv_values_copy(ctx, argv[1]) : JS_NewArray(ctx);
        CHECK(!JS_IsException(values), "cssom: a CSSFontFeatureValuesMap value list could not be allocated");
        if (!JS_IsArray(argv[1]))
            JS_SetPropertyUint32(ctx, values, 0, JS_DupValue(ctx, argv[1]));
        ffv_put(ctx, this_val, key, values);
        /* §12.2's IDL return type is `undefined`, which is what a browser answers. Web IDL §3.7.11.9's
           trailing sentence says a DECLARED `set` "must return this"; the two disagree and the declared return
           type is what the binding converts to, so the declaration wins. Recorded rather than resolved
           silently because a reader who re-derives it from §3.7.11.9 alone will return the receiver. */
        break;
    }
    }
    JS_FreeCString(ctx, key);
    return out;
}

/* ---- Web IDL §3.7.11's shared five, through core/idl_iter.h ----------------------------------------------- */

static int ffv_pair_count(JSContext *ctx, JSValueConst target)
{
    if (!css_font_feature_values_map_is(ctx, target)) return -1;   /* the receiver check */
    return (int)ffv_count(ctx, target);
}

/* §3.7.11.2's closure yields « key, value » per entry, the value being the sequence converted to a JS value —
   so a NEW Array, for the reason ffv_values_copy gives. */
static void ffv_pair_at(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    JSValue stored = ffv_entry_at(ctx, target, (uint32_t)i, 1);

    *key = ffv_entry_at(ctx, target, (uint32_t)i, 0);
    *value = ffv_values_copy(ctx, stored);
    JS_FreeValue(ctx, stored);
}

static const IdlPairIterOps FFV_PAIR_OPS = {
    ffv_pair_count, ffv_pair_at, "CSSFontFeatureValuesMap", false, /* maplike */ true
};

/* ---- the interface ---------------------------------------------------------------------------------------- */

JSValue css_font_feature_values_map_new(JSContext *ctx, const char *kind)
{
    JSValue proto, obj, arr;

    DCHECK(g_map_class != 0,
           "a CSSFontFeatureValuesMap was built before css_font_feature_values_init declared the interface");
    DCHECK(kind != NULL && css_font_feature_value_at_rule(kind),
           "a CSSFontFeatureValuesMap was minted for a block that is not one of css-fonts-4 §6.9.1's seven "
           "`<font-feature-value-type>` at-rules. The kind is what decides §12.2's `InvalidAccessError` count, "
           "so a map with no block behind it could not judge a `set` at all");
    proto = JS_GetClassProto(ctx, g_map_class);
    DCHECK(!JS_IsNull(proto),
           "a CSSFontFeatureValuesMap was built in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_map_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a CSSFontFeatureValuesMap could not be allocated");
    arr = JS_NewArray(ctx);
    CHECK(!JS_IsException(arr), "a CSSFontFeatureValuesMap's entry list could not be allocated");
    /* THE SLOTS NEVER MOVE — every member mutates the Array IN PLACE, `clear` included, because that is what
       makes each mutation a property write the per-flow COW delta already captures, and because §3.7.11.11's
       own note requires the map object to be PRESERVED across a clear. Defined once and unwritable, exactly as
       core/css/media_list.c's collection slot is. */
    JS_DefinePropertyValue(ctx, obj, g_atom_entries, arr, 0);
    JS_DefinePropertyValue(ctx, obj, g_atom_kind, JS_NewString(ctx, kind), 0);
    return obj;
}

void css_font_feature_values_init(JSContext *ctx)
{
    JSClassDef d = { "CSSFontFeatureValuesMap" };
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
    static const IdlArgType SET_ARGS[2] = { IDL_DOMSTRING, IDL_UNSIGNED_LONG_OR_SEQUENCE };
    static const IdlArgType NONE[1] = { IDL_ANY };

    if (g_map_class) return;   /* one AGENT, one class and one set of pool entries */
    JS_NewClassID(JS_GetRuntime(ctx), &g_map_class);
    JS_NewClass(JS_GetRuntime(ctx), g_map_class, &d);
    agent_state_class("element", &g_map_class,
                      "css-fonts-4 §12.2 \"The CSSFontFeatureValuesRule interface\"'s CSSFontFeatureValuesMap "
                      "class, and this component's latch");
    g_entries_key = JS_NewSymbol(ctx, "cssFontFeatureValuesEntries", false);
    CHECK(!JS_IsException(g_entries_key), "the CSSFontFeatureValuesMap entry-slot key allocation failed");
    g_atom_entries = JS_ValueToAtom(ctx, g_entries_key);
    CHECK(g_atom_entries != JS_ATOM_NULL, "the CSSFontFeatureValuesMap entry-slot key could not be interned");
    g_kind_key = JS_NewSymbol(ctx, "cssFontFeatureValuesKind", false);
    CHECK(!JS_IsException(g_kind_key), "the CSSFontFeatureValuesMap kind-slot key allocation failed");
    g_atom_kind = JS_ValueToAtom(ctx, g_kind_key);
    CHECK(g_atom_kind != JS_ATOM_NULL, "the CSSFontFeatureValuesMap kind-slot key could not be interned");
    g_id_get = idl_method_id(ctx, ONE_STR, 1, js_ffv_member, FFV_GET);
    g_id_has = idl_method_id(ctx, ONE_STR, 1, js_ffv_member, FFV_HAS);
    g_id_set = idl_method_id(ctx, SET_ARGS, 2, js_ffv_member, FFV_SET);
    g_id_delete = idl_method_id(ctx, ONE_STR, 1, js_ffv_member, FFV_DELETE);
    g_id_clear = idl_method_id(ctx, NONE, 0, js_ffv_member, FFV_CLEAR);
    /* Web IDL §3.7.11's `entries`, `keys`, `values`, `forEach` and @@iterator, plus §3.7.11.2's MAP ITERATOR —
       the flag beside the ops is what states that this is §3.7.11 and not §3.7.9. The component name is the row
       whose release gives this interface's state back, which is `element` here for the reason every CSS
       component's is: core/dom/element.c is what calls this file's init and free. */
    g_pair_handle = idl_pair_iter_declare(ctx, "element", &FFV_PAIR_OPS);
    realm_declare_intrinsic(css_font_feature_values_install_proto);
}

void css_font_feature_values_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_map_class != 0,
           "a realm asked for CSSFontFeatureValuesMap.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_map_class);
    DCHECK(JS_IsNull(prev), "css_font_feature_values_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "CSSFontFeatureValuesMap.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CSSFontFeatureValuesMap");
    idl_install_accessor(ctx, proto, "size", js_ffv_size, 0, -1);
    idl_install_method(ctx, proto, "get", g_id_get);
    idl_install_method(ctx, proto, "has", g_id_has);
    idl_install_method(ctx, proto, "set", g_id_set);
    idl_install_method(ctx, proto, "delete", g_id_delete);
    idl_install_method(ctx, proto, "clear", g_id_clear);
    /* §3.7.11.2 through §3.7.11.6 — and @@iterator IS the `entries` function object, which that section states
       ("whose value is the function object that is the value of the entries property") and which the shared
       install performs. */
    idl_pair_iter_install(ctx, proto, g_pair_handle);
    JS_SetClassProto(ctx, g_map_class, proto);
}

void css_font_feature_values_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_map_class);

    DCHECK(!JS_IsNull(proto),
           "CSSFontFeatureValuesMap was installed in a realm that never ran its prototype install");
    /* §12.2 declares no constructor, so the interface object's call and construct both throw. */
    idl_define_global_property_reference(ctx, global, "CSSFontFeatureValuesMap",
                                        idl_interface_object(ctx, "CSSFontFeatureValuesMap", proto));
    JS_FreeValue(ctx, proto);
}

void css_font_feature_values_free(JSRuntime *rt)
{
    if (!g_map_class) return;   /* the prototype is the REALM's — released with its context */
    JS_FreeAtomRT(rt, g_atom_entries);
    g_atom_entries = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_entries_key);
    g_entries_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_kind);
    g_atom_kind = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_kind_key);
    g_kind_key = JS_UNDEFINED;
    g_id_get = g_id_has = g_id_set = g_id_delete = g_id_clear = -1;
    if (g_pair_handle >= 0) idl_pair_iter_release(g_pair_handle);
    g_pair_handle = -1;
    /* THE CLASS ID IS NOT RESET HERE — it is declared under `element`, whose release ends in agent_state_undo,
       which is this engine's ONE reset for a class id. AND THE CASCADE REACHED THIS FILE, which is the claim
       that entitles element_free's last line to put it back: element_free calls this, and this says so. See
       core/agent_state.h. */
    agent_state_reached("element");
}
