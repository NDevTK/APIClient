/* DOMTokenList — DOM §7.1.
 *
 * WHY IT MATTERS TO A SOLVER: `el.classList.contains('is-admin')` and `classList.add('open')` are how a bundle
 * gates a whole branch of its UI, and a branch is where endpoints live. It was absent, so `el.classList` was
 * undefined and the page's own `.contains` threw — every route behind it unreachable.
 *
 * THERE IS NO STORAGE HERE, AND THAT IS THE DESIGN. §7.1 defines a token list as a VIEW over a content
 * attribute: its tokens ARE the attribute's value split on whitespace, and every mutation re-serialises and
 * writes the attribute back. So this file holds no tokens. That is not a simplification — it is what makes
 * `el.className = 'a'; el.classList.contains('a')` true, and it is what makes the list per-flow for free: the
 * attribute goes through the DOM-mutation chokepoint, so a token added in one arm of a fork is invisible to its
 * sibling, and a class change runs the custom-element attribute reaction, both without a line here.
 *
 * [SameObject] IS AN IDENTITY, so the list object is cached on the element's wrapper: `el.classList ===
 * el.classList` is what the IDL states, and a page that stashes the list and mutates it later must be mutating
 * the same one. The cache rides the WRAPPER, which is per-flow like the wrapper is.
 *
 * `supports(token)` IS §7.1's ALGORITHM AND NOT ITS DATA: the validation steps run here, and the token SETS
 * come from core/html/supported_tokens.c because §7.1 says whose they are — "Specifications may define
 * supported tokens for a DOMTokenList's element and attribute name". See js_tl_supports. */
#include <string.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "solver/concolic.h"   /* an unknown NAME denotes its shape — see concolic_name_cstr */
#include "quickjs.h"
#include "solver/dom_cow.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/idl_indexed.h"
#include "core/dom/node.h"
#include "core/dom/dom_token_list.h"
#include "core/html/supported_tokens.h"   /* §7.1: "Specifications may define supported tokens …" — HTML's */

static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
/* `toggle(DOMString token, optional boolean force)` — the optional boolean is what makes the two-argument form
   mean something different from the one-argument form, so it is declared rather than sniffed. */
static const IdlArgType IDL_STR_BOOL[2] = { IDL_DOMSTRING, IDL_BOOLEAN };

/* PER REALM — §3.7 gives each its own, and here that decides ANSWERS: a C member runs in the realm that
   DEFINED it, so one shared prototype answers every document out of whichever realm built it first. Held in
   quickjs's per-context class-proto slot. */
static JSClassID g_tl_class;
/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. */
static int g_set_value_id = -1, g_item_id = -1, g_contains_id = -1, g_add_id = -1, g_remove_id = -1,
           g_toggle_id = -1, g_replace_id = -1, g_to_string_id = -1, g_supports_id = -1;
/* §7.1'S TOKEN-LIST REFLECTIONS — ONE LIST EXPANDED THREE TIMES: the id the engine indexes by, the IDL member
   name, and the CONTENT ATTRIBUTE that member is a view over. `classList` was the only one and its attribute
   was hardcoded inside list_owner, which is a second implementation of this file waiting to be written: a
   `relList` is the SAME object over `rel`, and `sandbox` the same over `sandbox`. A member and its attribute
   cannot drift because installing a member resolves it through this list. */
#define TOKEN_LIST_REFLECTIONS(X) \
    X(TL_CLASS,   "classList", "class")   \
    X(TL_REL,     "relList",   "rel")     \
    X(TL_SIZES,   "sizes",     "sizes")   \
    X(TL_SANDBOX, "sandbox",   "sandbox")
#define TL_ID(id, member, attr)     id,
#define TL_MEMBER(id, member, attr) member,
#define TL_ATTR(id, member, attr)   attr,
enum { TOKEN_LIST_REFLECTIONS(TL_ID) TL_N };
static const char *const TL_MEMBERS[TL_N] = { TOKEN_LIST_REFLECTIONS(TL_MEMBER) };
static const char *const TL_ATTRS[TL_N] = { TOKEN_LIST_REFLECTIONS(TL_ATTR) };

/* ONE [SameObject] CACHE SLOT PER REFLECTION, because `a.relList === a.relList` and `a.classList ===
   a.classList` are two identities on the same element and one slot would hand the second reader the first
   one's list — a list over the wrong attribute, with nothing to say so. */
static JSValue g_key[TL_N];                /* the [SameObject] cache slot on an element's wrapper */
static JSValue g_owner_key = JS_UNDEFINED; /* the element a list is a view over */
static JSValue g_which_key = JS_UNDEFINED; /* WHICH reflection it is — the list's own attribute rides here */
static int     g_ready;

/* ---- the view ---------------------------------------------------------------------------------------------- */
/* Which content attribute this list reflects. One list kind so far (`class`), but the name is carried on the
   list rather than assumed, because §7.1's other reflections (`rel`, `sandbox`) are the same object over a
   different attribute and a hard-coded "class" would be a second implementation the day one lands. */
static lxb_dom_element_t *list_owner(JSContext *ctx, JSValueConst this_val, const char **attr)
{
    JSValue owner, which;
    JSAtom k;
    lxb_dom_node_t *n;
    int32_t w = TL_CLASS;

    if (!JS_IsObject(this_val)) return NULL;
    /* WHICH ATTRIBUTE THIS LIST IS A VIEW OVER, read off the list's own slot. Every member reads it through
       here, so there is no member left that could assume `class`. */
    k = JS_ValueToAtom(ctx, g_which_key);
    if (k == JS_ATOM_NULL) return NULL;
    if (JS_GetOwnSlot(ctx, &which, this_val, k) > 0) JS_ToInt32(ctx, &w, which);
    else which = JS_UNDEFINED;
    JS_FreeValue(ctx, which);
    JS_FreeAtom(ctx, k);
    DCHECK(w >= 0 && w < TL_N, "a DOMTokenList carries a reflection id §7.1 does not name — the id and the "
                               "attribute come from one list, so a value outside it is a list this component "
                               "did not build");
    if (attr) *attr = TL_ATTRS[w];
    k = JS_ValueToAtom(ctx, g_owner_key);
    if (k == JS_ATOM_NULL) return NULL;
    if (JS_GetOwnSlot(ctx, &owner, this_val, k) <= 0) owner = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    n = node_of(owner);
    JS_FreeValue(ctx, owner);
    return n && n->type == LXB_DOM_NODE_TYPE_ELEMENT ? lxb_dom_interface_element(n) : NULL;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* DOM §1.2 "Ordered sets" ordered set parser: the attribute's value split on ASCII whitespace, duplicates
   dropped. The parser is §1.2's and NOT §7.1's — §7.1 "Interface DOMTokenList" only says which string to run it
   over — and citing the consumer for the algorithm sends a reader to a section that does not contain it. The
   caller gets the value it must free and a cursor it walks with token_next. */
static const char *list_value(lxb_dom_element_t *el, const char *attr, size_t *len)
{
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)attr, strlen(attr), len);
    if (!v) { *len = 0; return ""; }
    return (const char *)v;
}

/* Advance `*p` past whitespace and report the next token's bounds. false when there is none left. */
static bool token_next(const char **p, const char *end, const char **tok, size_t *tlen)
{
    const char *s = *p;
    while (s < end && is_ws(*s)) s++;
    if (s >= end) { *p = s; return false; }
    *tok = s;
    while (s < end && !is_ws(*s)) s++;
    *tlen = (size_t)(s - *tok);
    *p = s;
    return true;
}

static bool value_has(const char *v, size_t vlen, const char *tok, size_t tlen)
{
    const char *p = v, *end = v + vlen, *t;
    size_t l;
    while (token_next(&p, end, &t, &l))
        if (l == tlen && memcmp(t, tok, tlen) == 0) return true;
    return false;
}

/* DOM §1.2 "Ordered sets"' ORDERED SET PARSER DROPS DUPLICATES — step 2 makes `tokens` an ORDERED SET and step
   3 APPENDS to it, and appending to a set that already holds the token does nothing — and that is what this
   list IS, a token SET. `class="a a b"` is
   TWO tokens, so `length` is 2 and `item(1)` is `b`; walking the attribute's raw tokens made both of them
   answer 3 and `a`, which is `contains` and `length` disagreeing about the very same list.
   The set's members are the FIRST occurrences, so a token is in the set exactly when it does not appear in the
   value BEFORE itself — which the prefix ending at this token is, and which needs no allocation and no second
   pass. Every member that counts or indexes the list walks with this rather than with token_next; the two are
   deliberately different functions because the mutation walk wants the raw tokens (it dedups into its OUTPUT,
   which is what re-serialises the set). */
static bool set_next(const char *v, const char **p, const char *end, const char **tok, size_t *tlen)
{
    while (token_next(p, end, tok, tlen))
        if (!value_has(v, (size_t)(*tok - v), *tok, *tlen))
            return true;
    return false;
}

/* DOM §7.1 Interface DOMTokenList's TOKEN SET SIZE — the §1.2 "Ordered sets" parser run for its count, and one
   implementation of it because `length` and `item`'s unknown-index assert ask the SAME question. Two walks
   would be two chances for the size a member reports and the size a should-never-happen tests against to
   disagree, which is the one way that assert could be right about a list nobody has. */
static uint32_t set_size(const char *v, size_t vlen)
{
    const char *p = v, *end = v + vlen, *t;
    size_t tlen;
    uint32_t n = 0;

    while (set_next(v, &p, end, &t, &tlen)) n++;
    return n;
}

/* §7.1 "run the update steps": re-serialise the set and write it through the DOM chokepoint, so the write is
   per-flow and runs the attribute change steps exactly like a setAttribute the page wrote itself.
 *
 * STEP 1 IS AN EARLY RETURN, AND IT WAS NOT HERE. "If get an attribute by namespace and local name given null,
 * set's attribute name, and set's element returns null AND set's token set is empty, then return." So an
 * element with no `class` attribute at all, asked for a token set that ends up empty, gets NO attribute — where
 * this wrote `class=""` and thereby CREATED one. That is a visible difference: `hasAttribute("class")` flips,
 * the serialization grows an attribute, and every layer below sees a write that the standard says does not
 * happen — an attribute change step, a mutation record, a custom element's attributeChangedCallback, and an
 * entry in the running flow's DOM delta.
 *
 * NAMED RESIDUAL — the write carries NO TAINT. NOT COVERED: an unknown token's provenance. `setAttribute`
 * (element.c), `input.value =` and a `dataset` write all hand dom_cow_set_attribute the source beside the
 * shape's bytes, so `el.setAttribute('class', x)` keeps x's identity in the (element, name) shadow map and
 * `el.classList.add(x)` does not. NEXT DIFF: list_write takes the taint its caller holds — which for `value =`
 * is one value and is the whole of it, and for a MUTATION is not, because the serialized attribute is composed
 * of several tokens of which only some are unknown and dom_cow_set_attribute carries ONE JSValue for the whole
 * attribute; so the mutation half needs a per-token key in that map before it can be honest, and half of it is
 * worse than none. HOW ITS ABSENCE SHOWS: `classList.add(location.hash.slice(1))` followed by a sink fed from
 * `el.className` reports concrete bytes with no source, so no @S search starts for a breakout that is real. */
static void list_write(lxb_dom_element_t *el, const char *attr, const char *val, size_t len)
{
    size_t have = 0;
    if (len == 0 && !lxb_dom_element_get_attribute(el, (const lxb_char_t *)attr, strlen(attr), &have))
        return;                       /* update steps, step 1 */
    dom_cow_set_attribute(el, attr, val, len, JS_UNDEFINED);
}

/* THE BYTES OF ONE TOKEN ARGUMENT, WHICH MAY BE UNKNOWN EXTERNAL INPUT — the same question `value =` and
   `supports()` already ask, asked once so a third member cannot answer it differently.
   IT IS NOT AN OPTIMISATION, IT IS THE DIFFERENCE BETWEEN A LIST AND A DEAD FLOW. An IDL_DOMSTRING position
   is IDL_CONCOLIC_CROSSES, so the declaration hands an unknown to the body AS ITSELF, and a raw
   JS_ToCStringLen on one ABORTS at the C boundary by design — js_force_tostring's DFAIL says it outright, "a
   `const char *` cannot carry a concolic". So `classList.contains(x)` and `classList.add(x)` over external
   input ended the document at the coercion, which is precisely the read this file exists for: its own banner
   names `el.classList.contains('is-admin')` as how a bundle gates a whole branch of its UI.
   An unknown denotes its SHAPE, a real string stable per source, so one source is one stable class. Anything
   else converts with its REAL length, because a token is compared as BYTES and a NUL inside one is not a
   terminator. OWNED either way: free with JS_FreeCString. */
static const char *token_bytes(JSContext *ctx, JSValueConst v, size_t *len)
{
    const char *s;

    if (!concolic_is(v)) return JS_ToCStringLen(ctx, len, v);
    s = concolic_name_cstr(ctx, v);
    *len = s ? strlen(s) : 0;
    return s;
}

/* §7.1: a token must be non-empty and contain no ASCII whitespace. Both are the spec's own errors, not a
   silent no-op — a page that passes "a b" to add() is asking for something the list cannot represent. */
static int token_check(JSContext *ctx, const char *tok, size_t tlen)
{
    size_t i;
    if (tlen == 0) {
        JS_ThrowDOMException(ctx, "SyntaxError", "the token is empty");
        return -1;
    }
    for (i = 0; i < tlen; i++)
        if (is_ws(tok[i])) {
            JS_ThrowDOMException(ctx, "InvalidCharacterError", "the token contains whitespace");
            return -1;
        }
    return 0;
}

/* ---- the members ------------------------------------------------------------------------------------------- */
/* §7.1 `readonly attribute unsigned long length` — "the number of tokens", which is the token set's size. It
   is minted UNSIGNED because the IDL says the type is, not because a class attribute could hold 2**31 tokens:
   a `long` spelling of an `unsigned long` is the same disagreement between a declaration and its member that
   `item` carried below, and it is worth nothing to leave one of the pair right and the other wrong. */
static JSValue js_tl_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    const char *attr, *v;
    size_t vlen = 0;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);

    (void)magic;
    if (!el) return JS_NewUint32(ctx, 0);
    v = list_value(el, attr, &vlen);
    return JS_NewUint32(ctx, set_size(v, vlen));
}

/* §7.1 `value` — the attribute itself, and its setter is the attribute's setter. It is the stringifier too. */
static JSValue js_tl_value(JSContext *ctx, JSValueConst this_val, int magic)
{
    const char *attr, *v;
    size_t vlen = 0;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);

    (void)magic;
    if (!el) return JS_NewString(ctx, "");
    v = list_value(el, attr, &vlen);
    return JS_NewStringLen(ctx, v, vlen);
}

static JSValue js_tl_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv; (void)magic;
    return js_tl_value(ctx, this_val, 0);
}

static JSValue js_tl_set_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *attr, *s;
    size_t slen = 0;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);

    (void)magic;
    if (!el) return JS_UNDEFINED;
    /* the declaration passes UNKNOWN input through as itself; an unknown denotes its SHAPE, so
       `el.classList.value = x` writes one stable value per source instead of ending the document at the
       coercion — token_bytes, the same answer `contains`, `supports` and every mutation give. A CONCRETE value
       arrives with its real length there, which a strlen here did not: `class` may legitimately carry a NUL. */
    s = token_bytes(ctx, val, &slen);
    if (!s) return JS_EXCEPTION;
    list_write(el, attr, s, slen);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* §7.1's `item(index)` method steps: "If index is equal to or greater than this's token set's size, then
   return null", then "Return this's token set[index]". That null past the end is what makes the OPERATION
   different from the indexed property getter this same body backs — §7.1's supported property indices "are the
   numbers in the range zero to object's token set's size − 1", so `list[size]` is undefined where
   `list.item(size)` is null.

   THE INDEX IS `unsigned long`, AND THE BODY NO LONGER RE-STATES THAT. §7.1 writes
   `getter DOMString? item(unsigned long index)` — the comment above tl_item already quoted that line while the
   declaration below said `long`, which is how the disagreement was readable at all. This read used to be
   `JS_ToInt64` plus `if (want < 0) return JS_NULL`: Web IDL §3.2.4.5 long converts with
   ConvertToInt(V, 32, "signed"), whose final step is "If signedness is 'signed' and x ≥ 2^(bitLength−1), then
   return x − 2^bitLength", so `item(2**31)` denoted −2147483648 where §3.2.4.6 unsigned long's
   ConvertToInt(V, 32, "unsigned") denotes 2147483648 — and the body's negative branch is what turned that back
   into the null a browser answers. THE COMPENSATION IS WHY THE WRONG TYPE SURVIVED: a token set cannot hold
   2**31 tokens, so every value at or past 2**31 is past the end under either sign and nothing a page can write
   observes the difference. The declaration is the spec of the conversion; a body re-deriving the sign is the
   second copy of §3.2.4.9 Abstract operations' arithmetic that idl_args.c exists to prevent. */
static JSValue js_tl_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *attr, *v, *p, *end, *t;
    size_t vlen = 0, tlen;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);
    uint32_t want = 0, n = 0;

    (void)magic;
    if (!el) return JS_NULL;
    DCHECK(argc >= 1, "§7.1's `item` reached its body with no argument — its IDL argument is required, so the "
                      "declaration's own argument-count check is what should have refused the call");
    v = list_value(el, attr, &vlen);
    if (concolic_is(argv[0])) {
        /* AN UNKNOWN INDEX, and it reaches the body unconverted because §3.2's conversion is a boundary
           unknown external input crosses AS ITSELF (idl_concolic_rule answers IDL_CONCOLIC_CROSSES for every
           integer type). The EMPTY token set is the one size at which that has an answer rather than a fork:
           step 1 returns null for every index at or past the size, and at size 0 that is every index there is.
           READING IT WITH `JS_ToInt64` INSTEAD — which is what stood here — IS THE SHAPE idl_args.h BANS BY
           NAME: a concolic is a real JSObject, so ToNumber reaches ToPrimitive and runs a getter from a plain
           C frame, which this engine aborts on somewhere inside the coercion rather than here at the member. */
        DCHECK(set_size(v, vlen) == 0,
               "§7.1's `item` was given an UNKNOWN index into a NON-EMPTY DOMTokenList — every token in it is a "
               "distinct answer, so the read must FORK one flow per supported index (plus the null arm for an "
               "index past the end) instead of deciding it here");
        return JS_NULL;
    }
    JS_ToUint32(ctx, &want, argv[0]);   /* the declaration ran §3.2.4.6 unsigned long: already [0, 2**32-1] */
    p = v; end = v + vlen;
    while (set_next(v, &p, end, &t, &tlen)) {
        if (n == want) return JS_NewStringLen(ctx, t, tlen);
        n++;
    }
    return JS_NULL;
}

static JSValue js_tl_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *attr, *v, *tok;
    size_t vlen = 0, tlen = 0;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);
    JSValue out;
    bool has;

    (void)magic;
    if (!el || argc < 1) return JS_FALSE;
    tok = token_bytes(ctx, argv[0], &tlen);
    if (!tok) return JS_EXCEPTION;
    v = list_value(el, attr, &vlen);
    has = value_has(v, vlen, tok, tlen);
    JS_FreeCString(ctx, tok);
    out = JS_NewBool(ctx, has);
    /* AN UNKNOWN TOKEN IS AN UNKNOWN ANSWER, for the reason `supports` states one member up: the membership
       was decided against the SHAPE, which is a real string and is nobody's class, so the concrete example is
       `false` — but the DOMAIN still permits both, and a concrete `false` here would decide
       `if (el.classList.contains(x))` for the flow and delete the true arm. That arm is the whole point: a
       bundle gates its admin UI on exactly this call. */
    if (concolic_is(argv[0]))
        out = concolic_builtin_hook(ctx, argv[0], "DOMTokenList.contains", out);
    return out;
}

/* §7.1 `boolean supports(DOMString token)` — "Let result be the return value of validation steps called with
 * token. Return result." The four validation steps are here and the TOKEN SETS ARE NOT, because §7.1 says who
 * owns them in its own sentence: "Specifications may define supported tokens for a DOMTokenList's element and
 * attribute name." So core/html/supported_tokens.c answers which set a (element, attribute name) pair has and
 * what is in it, and this body is step 1's throw, step 2's lowercasing and steps 3-4's answer.
 *
 * WHY A PAGE READS IT, AND WHY ITS ABSENCE COST MORE THAN A MEMBER. `document.createElement("link").relList
 * .supports("modulepreload")` and `.supports("preload")` are how every modern bundler's chunk loader decides
 * which `rel` it will emit for a lazy chunk — CLAUDE.md §What-the-tool-produces names the lazy chunk as a
 * headline target. With the member absent the unguarded spelling threw `supports is not a function` and ended
 * the flow, and the guarded spelling (`relList.supports && relList.supports(x)`) took the OTHER branch on a
 * CONCRETE `undefined`, so the arm was not even forked — a whole loader path lost with nothing to say so.
 *
 * STEP 1 IS A TypeError AND NOT A `false`, and the difference is the whole reason the answer below is
 * three-state: `classList.supports("x")` and `link.sizes.supports("any")` throw in a browser because no
 * specification defines supported tokens for `class` or for `sizes`, while
 * `link.relList.supports("stylesheet")` answers false. A single boolean would have to pick one of those and
 * would be wrong about the other. */
static JSValue js_tl_supports(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *attr = NULL, *tok;
    size_t tlen = 0, i;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);
    HtmlSupportedToken supported;
    char *lower;
    JSValue out;

    (void)magic;
    DCHECK(argc >= 1, "§7.1 `supports(token)` ran with no argument — Web IDL §3.6's arity check belongs to the "
                      "declaration and throws a TypeError before the body runs, so an empty argument list here "
                      "is a member installed without the declared argument it converts");
    /* Web IDL §3.7.6: an operation on an interface whose receiver is not a platform object implementing it is a
       TypeError. Every list this component mints carries its element, so an unresolvable owner IS that case. */
    if (!el)
        return JS_ThrowTypeError(ctx, "supports called on a receiver that is not a DOMTokenList with an "
                                      "associated element");
    /* An unknown token denotes its SHAPE, the same rule `value =` and `contains` follow — one spelling, in
       token_bytes, because three members answering the same question three times is three chances to differ. */
    tok = token_bytes(ctx, argv[0], &tlen);
    if (!tok) return JS_EXCEPTION;
    /* Validation step 2: "Let lowercaseToken be token, in ASCII lowercase." */
    lower = malloc(tlen + 1);
    CHECK(lower != NULL, "DOMTokenList: OOM lowercasing a supports() token");
    for (i = 0; i < tlen; i++) {
        char c = tok[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[tlen] = 0;
    JS_FreeCString(ctx, tok);
    supported = html_supported_token(el, attr, lower, tlen);
    free(lower);
    /* Validation step 1. It is decided by the ELEMENT and the ATTRIBUTE NAME alone, which is why it is asked
       after the conversion and not before it: §7.1 converts the argument first (Web IDL runs the declaration's
       coercion, and a `toString` that throws must throw) and only then looks at the pair. */
    if (supported == HTML_TOKENS_UNDEFINED)
        return JS_ThrowTypeError(ctx, "no supported tokens are defined for the `%s` attribute of this element",
                                 attr);
    out = JS_NewBool(ctx, supported == HTML_TOKENS_PRESENT);
    /* AN UNKNOWN TOKEN IS AN UNKNOWN ANSWER. The membership was decided against the shape, which is a real
       string and is no keyword, so the concrete example is `false` — but the DOMAIN still permits both, and a
       concrete `false` here would decide `if (list.supports(x))` for the flow and delete the true arm. The
       result therefore carries the operand's source, exactly as every other builtin over unknown input does. */
    if (concolic_is(argv[0]))
        out = concolic_builtin_hook(ctx, argv[0], "DOMTokenList.supports", out);
    return out;
}

/* ONE TOKEN ARGUMENT, held as the bytes the walk compares against. A list of these is what makes `add` and
   `remove` variadic without a second mutation body. */
typedef struct { const char *s; size_t len; } TlToken;

/* Is `tok` one of the tokens THIS CALL named? §7.1 remove's step 2 is "for each token of tokens: remove token
   from this's token set", so the walk asks the whole argument list rather than one position. */
static bool tl_named(const TlToken *a, int na, const char *tok, size_t tlen)
{
    int i;
    for (i = 0; i < na; i++)
        if (a[i].len == tlen && memcmp(a[i].s, tok, tlen) == 0) return true;
    return false;
}

/* THE ONE MUTATION, under four names. add/remove/toggle/replace differ only in which tokens they mean to be
   present afterwards, so they build the new value the same way: walk the existing tokens, decide each, then
   append what is newly wanted. Four bodies would be four chances to disagree about whitespace and duplicates.
   magic 0 = add, 1 = remove, 2 = toggle, 3 = replace.
 *
 * §7.1 `add(tokens…)` AND `remove(tokens…)` ARE `DOMString... tokens`, AND THEIR THREE STEPS ARE THREE PASSES
 * IN THAT ORDER — step 1 validates EVERY token, step 2 applies EVERY token, step 3 runs the update steps ONCE.
 * Both halves of that order are observable and neither is what a token-at-a-time loop produces:
 * `classList.add('a','')` must throw a "SyntaxError" DOMException having added NOTHING, and
 * `classList.add('a','b')` must produce ONE attribute change step, one mutation record and one delta entry
 * rather than two. So every token is collected, then every token is checked, and only then does the walk
 * below start. toggle and replace are the same shape at a fixed token count: toggle's steps 1-2 and replace's
 * steps 1-2 are those same two throws over the one (or two) tokens their IDL lists. */
static JSValue js_tl_mutate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *attr, *v, *p, *end, *t;
    size_t vlen = 0, tlen, tok_bytes = 0;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);
    char *out = NULL;
    size_t out_len = 0, out_cap = 0;
    JSValue result = JS_UNDEFINED;
    TlToken *a = NULL;
    int i, na;
    bool force_given = false, force = false, present = false, want;

    /* WHICH ARGUMENTS ARE TOKENS, read off the same IDL the declaration states rather than assumed. `add` and
       `remove` are `DOMString... tokens`, so EVERY argument is a token and how many there are is the page's
       to decide; `toggle(token, optional force)` has one token and a boolean beside it; `replace(token,
       newToken)` has two tokens. */
    na = (magic == 0 || magic == 1) ? argc : (magic == 3 ? 2 : 1);
    /* Web IDL §3.6 "Overload resolution algorithm" step 5 throws the TypeError for a call that reached fewer
       than the required positions, and it belongs to the DECLARATION — so toggle or replace arriving here with
       fewer is a member installed without the arguments it converts, not a page doing something. add and
       remove require NOTHING, because Web IDL §2.5.8 "Overloading"'s compute the effective overload set ends by
       appending the empty tuple for a final variadic argument: `classList.add()` is a legal call. */
    DCHECK(magic != 2 || argc >= 1,
           "§7.1 `toggle(token, optional force)` ran with no token — Web IDL §3.6 \"Overload resolution "
           "algorithm\" step 5's required-argument check belongs to the declaration and throws before this "
           "body runs, so an empty argument list here is a member installed without the token it converts");
    DCHECK(magic != 3 || argc >= 2,
           "§7.1 `replace(token, newToken)` ran with fewer than two arguments — Web IDL §3.6 \"Overload "
           "resolution algorithm\" step 5's required-argument check belongs to the declaration and throws "
           "before this body runs, so a short argument list here is a member installed without its positions");
    if (!el) return (magic == 2 || magic == 3) ? JS_FALSE : JS_UNDEFINED;
    if (na) {
        a = calloc((size_t)na, sizeof *a);
        CHECK(a != NULL, "DOMTokenList: OOM collecting a token-list mutation's tokens");
    }
    for (i = 0; i < na; i++) {
        a[i].s = token_bytes(ctx, argv[i], &a[i].len);
        if (!a[i].s) { result = JS_EXCEPTION; goto out; }
        tok_bytes += a[i].len;
    }
    /* §7.1 add/remove STEP 1, and toggle's steps 1-2 / replace's steps 1-2 — a SEPARATE PASS over the whole
       list, so a bad token in any position leaves the list untouched instead of half-mutated. */
    for (i = 0; i < na; i++)
        if (token_check(ctx, a[i].s, a[i].len) < 0) { result = JS_EXCEPTION; goto out; }
    if (magic == 2 && argc > 1 && !JS_IsUndefined(argv[1])) {
        force_given = true;
        /* §7.1 toggle's steps 3-5 branch on `force`, and this read is a plain coercion of a value that is
           ALREADY a real boolean: an IDL_BOOLEAN position is IDL_CONCOLIC_FORKS, so the conversion in
           core/idl_args.c asked §7.1.2 ToBoolean of it at the BRANCH seam before this body was entered — the
           same gate `if (flags.beta)` would ask, filed under the same key — and placed one truth value per
           world. `toggle(cls, flags.beta)` over unknown input therefore reaches here twice, in two
           flows, one adding the class and one removing it — which is what this line used to name as the
           residual it could not build. It is NOT a body's own fork and must not become one: the ask belongs to
           the boundary, where forty-odd members share it, and a second one here would fork a value that has
           already been decided. */
        force = JS_ToBool(ctx, argv[1]);
    }
    v = list_value(el, attr, &vlen);
    if (na) present = value_has(v, vlen, a[0].s, a[0].len);
    /* §7.1 toggle STEPS 3.2 AND 5 RETURN WITHOUT RUNNING THE UPDATE STEPS, and that is the whole difference
       between a no-op and a write. `toggle(t, true)` on a token that is already there answers true at step 3.2;
       `toggle(t, false)` on one that is absent answers false at step 5. Neither touches the token set, so
       neither may touch the attribute — this ran the update steps regardless, so a no-op toggle rewrote `class`
       with its own current value and produced an attribute change step, a mutation record and a delta entry a
       browser never produces. The condition is exactly "force was given and it already agrees with reality". */
    if (magic == 2 && force_given && force == present) {
        result = JS_NewBool(ctx, force);
        goto out;
    }
    /* §7.1 replace STEP 3 RETURNS WITHOUT RUNNING THE UPDATE STEPS — "If this's token set does not contain
       token, then return false" — the same shape as toggle's steps 3.2 and 5 above, and the spec says so in
       its own words a line later ("The update steps are not always run for replace() for web compatibility").
       Without it a replace naming an absent token re-serialised the attribute anyway, so `class="a  a"` became
       `class="a"` and produced an attribute change step, a mutation record, a custom element's
       attributeChangedCallback and a delta entry that a browser never produces. */
    if (magic == 3 && !present) {
        result = JS_FALSE;
        goto out;
    }
    /* §7.1 toggle's answer is whether the token is present AFTERWARDS, and with `force` it is force itself. */
    want = (magic == 0) ? true
         : (magic == 1) ? false
         : (magic == 2) ? (force_given ? force : !present)
         : true;                          /* replace: step 3 above already returned for an absent token */

    /* Every kept token costs at most its own bytes plus one separator, and the source separated it by at least
       one byte, so the existing set fits in `vlen`; the appended (or substituted) tokens cost their bytes plus
       one separator each. */
    out_cap = vlen + tok_bytes + (size_t)na + 2;
    out = malloc(out_cap);
    CHECK(out != NULL, "DOMTokenList: OOM building the updated token set");
    p = v; end = v + vlen;
    while (token_next(&p, end, &t, &tlen)) {
        const char *keep = t;
        size_t keep_len = tlen;
        if (magic == 3) {
            if (tlen == a[0].len && memcmp(t, a[0].s, tlen) == 0) {
                keep = a[1].s; keep_len = a[1].len;   /* §7.1 replace step 4, in place */
            }
        } else if (!want && tl_named(a, na, t, tlen)) {
            continue;                     /* remove (ANY of the named tokens), or toggle to absent */
        }
        /* DOM §1.2 "Ordered sets"' set semantics: a token already emitted is not emitted twice. */
        if (value_has(out, out_len, keep, keep_len)) continue;
        if (out_len) out[out_len++] = ' ';
        memcpy(out + out_len, keep, keep_len);
        out_len += keep_len;
    }
    /* §7.1 add's step 2 (and toggle's step 4): APPEND each named token, IN THE ORDER THE PAGE WROTE THEM, to
       a SET — so `add('a','b')` with `a` already there appends `b` alone, and `add('c','c')` appends one `c`,
       because appending to a set that already holds the token does nothing. */
    if (want && magic != 3)
        for (i = 0; i < na; i++) {
            if (value_has(out, out_len, a[i].s, a[i].len)) continue;
            if (out_len) out[out_len++] = ' ';
            memcpy(out + out_len, a[i].s, a[i].len);
            out_len += a[i].len;
        }
    /* §7.1 add/remove STEP 3, ONCE — never once per token. It runs even for `classList.add()`, which names no
       token at all: step 1 and step 2 iterate an empty list and step 3 still runs the update steps, so an
       element carrying `class="a  a  b"` is re-serialised to `class="a b"`. That is the spec's own
       normalisation and not a stray write — the update steps' step 1 is what keeps it from CREATING an
       attribute on an element that has none. */
    list_write(el, attr, out, out_len);
    if (magic == 2)      result = JS_NewBool(ctx, want);
    else if (magic == 3) result = JS_TRUE;   /* §7.1 replace step 6 — step 3 owns the absent case */
out:
    free(out);
    for (i = 0; i < na; i++)
        if (a[i].s) JS_FreeCString(ctx, a[i].s);
    free(a);
    return result;
}

/* §7.1 `getter DOMString? item(unsigned long index)` — the INDEXED PROPERTY GETTER, which is what `list[0]`
   is. Its backing is the same attribute every other member reads, so nothing here caches and nothing goes
   stale: the index is resolved against the class attribute at the moment it is asked. */
static uint32_t tl_length(JSContext *ctx, JSValueConst self)
{
    JSValue n = js_tl_length(ctx, self, 0);
    uint32_t v = 0;
    JS_ToUint32(ctx, &v, n);
    JS_FreeValue(ctx, n);
    return v;
}

/* The index this hands js_tl_item is a `uint32_t` the INDEXED-PROPERTY machinery already resolved, so the
   unknown-index arm in that body is unreachable FROM HERE and reachable only from the MEMBER — `list.item(x)`
   with an x the conversion crossed. That is a statement about which caller needs the arm, not an invariant to
   assert: a DCHECK here would be testing that JS_NewUint32 one line down returns a number, which is a fact
   about quickjs's constructor rather than about this file, and an assert whose condition the line above it
   establishes teaches a reader nothing when it never fires. */
static JSValue tl_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue idx = JS_NewUint32(ctx, i), r;
    JSValueConst argv[1];
    argv[0] = idx;
    r = js_tl_item(ctx, self, 1, argv, 0);
    JS_FreeValue(ctx, idx);
    return JS_IsNull(r) ? (JS_FreeValue(ctx, r), JS_UNDEFINED) : r;   /* past the end is not a property */
}

static const IdlIndexedDecl TOKEN_LIST_INDEXED = { "DOMTokenList", tl_length, tl_item, NULL };

/* ---- the interface ----------------------------------------------------------------------------------------- */
/* EVERY `[SameObject] readonly attribute DOMTokenList` REFLECTION, under one getter whose MAGIC is which one —
   §4.9's `classList`, §4.6.7's `relList`, §4.2.4's `sizes`, §4.8.5's `sandbox`. They differ only in the content
   attribute the list views, which the list carries, so one getter is not a shortcut: four bodies would be four
   chances to disagree about the cache, the owner slot and the attribute.
   [SameObject] is the SAME object every time, cached on the element's wrapper (so per-flow, like the wrapper).
   A fresh list per read would make `a.relList === a.relList` false, and a page that keeps the list and mutates
   it later would be mutating a corpse. */
static JSValue js_el_token_list(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k, ok;
    JSValue list;

    DCHECK(g_ready, "a token-list reflection was read before dom_token_list_init built its prototype");
    DCHECK(magic >= 0 && magic < TL_N, "a token-list reflection was installed with a magic §7.1 does not name");
    if (!JS_IsObject(this_val)) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key[magic]);
    if (k == JS_ATOM_NULL) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &list, this_val, k) <= 0) list = JS_UNDEFINED;
    if (!JS_IsObject(list)) {
        JS_FreeValue(ctx, list);
        JSValue tl_proto = dom_token_list_proto(ctx);
        list = idl_indexed_new(ctx, tl_proto, &TOKEN_LIST_INDEXED);
        JS_FreeValue(ctx, tl_proto);
        CHECK(!JS_IsException(list), "DOMTokenList: OOM allocating a token-list reflection");
        ok = JS_ValueToAtom(ctx, g_owner_key);
        CHECK(ok != JS_ATOM_NULL, "the DOMTokenList owner key could not be reached");
        JS_DefinePropertyValue(ctx, list, ok, JS_DupValue(ctx, this_val), 0);
        JS_FreeAtom(ctx, ok);
        ok = JS_ValueToAtom(ctx, g_which_key);
        CHECK(ok != JS_ATOM_NULL, "the DOMTokenList reflection key could not be reached");
        JS_DefinePropertyValue(ctx, list, ok, JS_NewInt32(ctx, magic), 0);
        JS_FreeAtom(ctx, ok);
        JS_DefinePropertyValue(ctx, (JSValue)this_val, k, JS_DupValue(ctx, list), 0);
    }
    JS_FreeAtom(ctx, k);
    return list;
}

void dom_token_list_install_reflection(JSContext *ctx, JSValueConst proto, const char *member)
{
    int i;

    /* THE MEMBER INSTALLED IS THE ONE THE INTERFACE ASKED FOR. `TL_MEMBERS[i]` is the same bytes — strcmp just
       said so — and naming the table cell instead said, to anything reading this line, that a call installs all
       four of §7.1's reflections on whichever prototype it was handed. Which member and which prototype are
       both the CALLER's, and an install a reader cannot attribute is an install a reader cannot review. */
    for (i = 0; i < TL_N; i++)
        if (!strcmp(TL_MEMBERS[i], member)) {
            idl_install_accessor(ctx, proto, member, js_el_token_list, i, -1);
            return;
        }
    DFAIL("an interface asked for a token-list reflection §7.1 does not name — the member name and the content "
          "attribute it views come from one list, and a name outside it has no attribute to be a view over");
}

void dom_token_list_init(JSContext *ctx)
{
    JSClassDef d = { "DOMTokenList" };

    DCHECK(!g_ready, "dom_token_list_init ran twice — the interface is declared once per AGENT");
    {
        int i;
        for (i = 0; i < TL_N; i++) {
            g_key[i] = JS_NewSymbol(ctx, TL_MEMBERS[i], false);
            CHECK(!JS_IsException(g_key[i]), "a DOMTokenList [SameObject] slot key could not be allocated");
        }
    }
    g_owner_key = JS_NewSymbol(ctx, "tokenListOwner", false);
    g_which_key = JS_NewSymbol(ctx, "tokenListReflection", false);
    CHECK(!JS_IsException(g_which_key) && !JS_IsException(g_owner_key),
          "the DOMTokenList slot keys could not be allocated");
    JS_NewClassID(JS_GetRuntime(ctx), &g_tl_class);
    JS_NewClass(JS_GetRuntime(ctx), g_tl_class, &d);
    g_set_value_id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_tl_set_value, 0);
    /* §7.1 writes `getter DOMString? item(unsigned long index)` and carries NO [EnforceRange], so §3.2.4.9
       Abstract operations' ConvertToInt modulo IS the specified behaviour and there is nothing here to throw.
       The type states the SIGN, which is the whole of what it decides — see js_tl_item for the negative branch
       this replaced and for why nothing could observe that it was wrong. */
    g_item_id = idl_method_id(ctx, (const IdlArgType[]){ IDL_UNSIGNED_LONG }, 1, js_tl_item, 0);
    g_contains_id = idl_method_id(ctx, IDL_1STR, 1, js_tl_contains, 0);
    /* §7.1 `undefined add(DOMString... tokens)` and `undefined remove(DOMString... tokens)` — THE TAIL IS
       DECLARED, so the body takes as many tokens as the page passed. It declared ONE, and Web IDL §3.6
       "Overload resolution algorithm" step 3 makes argcount min(maxarg, n): `classList.add('a','b')` reached
       the body with argc == 1, the second argument was never even CONVERTED, and the class was DROPPED WITH
       NOTHING TO SAY SO — no throw, no record, and a page whose CSS selector then never matches, whose
       `contains` answers wrong, and whose gated branch the solver takes down the wrong arm.
       AND THE IDL AUDIT CANNOT SEE THIS, WHICH A COMMENT HERE ONCE SAID IT COULD. idlgen.mjs compares the
       spec's member NAMES and KINDS against what a component installs, so an installed `add` reads as present
       whatever its signature says — the interface reported "complete" for as long as the gap existed. A
       signature is checked by READING THE IDL, and the check that survives is that the declaration states the
       tail rather than that a comment describes it.
       THE ARITY FOLLOWS FROM THE SAME PLACE. Web IDL §3.7.7 "Operations" builds the function with "the length
       of the shortest argument list in the entries in S" at argument count 0, and §2.5.8 "Overloading"'s
       compute the effective overload set ends by appending the empty tuple for a final variadic argument — so
       `add.length` and `remove.length` are 0 and `classList.add()` is a legal call rather than a TypeError. */
    g_add_id = idl_method_id(ctx, IDL_1STR, 1, js_tl_mutate, 0);
    idl_variadic();
    g_remove_id = idl_method_id(ctx, IDL_1STR, 1, js_tl_mutate, 1);
    idl_variadic();
    g_toggle_id = idl_method_id(ctx, IDL_STR_BOOL, 2, js_tl_mutate, 2);
    idl_optional_from(1);   /* §7.1: `toggle(token, optional force)` */
    g_replace_id = idl_method_id(ctx, IDL_2STR, 2, js_tl_mutate, 3);
    g_supports_id = idl_method_id(ctx, IDL_1STR, 1, js_tl_supports, 0);   /* §7.1 `supports(token)` */
    /* §7.1's stringifier IS `value` — the same attribute under the operation's name, so it reads the same
       thing rather than formatting one. Its own body because a getter and a method have different shapes, and
       casting one to the other reads `magic` out of `argc`. */
    g_to_string_id = idl_method_id(ctx, IDL_1STR, 1, js_tl_to_string, 0);
    idl_optional_from(0);   /* §7.1's stringifier takes NO arguments — the declared one is `value`'s */
    g_ready = 1;
    realm_declare_intrinsic(dom_token_list_install_proto);
}

/* §7.1's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. */
void dom_token_list_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for DOMTokenList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_tl_class);
    DCHECK(JS_IsNull(prev), "dom_token_list_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "DOMTokenList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DOMTokenList");
    idl_install_accessor_no_user_code(ctx, proto, "length", js_tl_length, 0, -1);
    idl_install_accessor(ctx, proto, "value", js_tl_value, 0, g_set_value_id);
    idl_install_method(ctx, proto, "item", g_item_id);
    idl_install_method(ctx, proto, "contains", g_contains_id);
    /* 0, not 1 — §3.7.7 "Operations" takes the SHORTEST argument list in the effective overload set at
       argument count 0, and a final variadic argument puts the empty tuple in that set. */
    idl_install_method(ctx, proto, "add", g_add_id);
    idl_install_method(ctx, proto, "remove", g_remove_id);
    idl_install_method(ctx, proto, "toggle", g_toggle_id);
    idl_install_method(ctx, proto, "replace", g_replace_id);
    idl_install_method(ctx, proto, "supports", g_supports_id);
    idl_install_method(ctx, proto, "toString", g_to_string_id);
    /* §3.7.9 step 1.1: an interface with an indexed getter is iterable through %Array.prototype.values%, which is why
       `for (const c of el.classList)` is ordinary code — and had nothing. */
    idl_indexed_install_iterable(ctx, proto);
    idl_indexed_install_value_iterator(ctx, proto);   /* §7.1 `iterable<DOMString>` */
    JS_SetClassProto(ctx, g_tl_class, proto);
}

JSValue dom_token_list_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_tl_class);
    DCHECK(!JS_IsNull(proto),
           "DOMTokenList.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

void dom_token_list_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "DOMTokenList was installed before its prototype was built");
    {
        JSValue proto = dom_token_list_proto(ctx);
        ctor = idl_interface_object(ctx, "DOMTokenList", proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "DOMTokenList", ctor);
}

void dom_token_list_install_element(JSContext *ctx, JSValueConst element_proto)
{
    DCHECK(g_ready, "classList was installed before dom_token_list_init built its prototype");
    dom_token_list_install_reflection(ctx, element_proto, "classList");
}

void dom_token_list_free(JSRuntime *rt)
{
    if (!g_ready) return;
    {
        int i;
        for (i = 0; i < TL_N; i++) { JS_FreeValueRT(rt, g_key[i]); g_key[i] = JS_UNDEFINED; }
    }
    JS_FreeValueRT(rt, g_owner_key);
    JS_FreeValueRT(rt, g_which_key);
    /* the prototypes are the REALMS' — released with their contexts */
    g_owner_key = g_which_key = JS_UNDEFINED;
    g_ready = 0;
}
