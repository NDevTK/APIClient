/* HTML §4.10.19.7 "Autofill" — see autofill.h for why this is a component and not a reflection row. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/idl_args.h"
#include "core/html/html_form.h"      /* §4.10.5.1's `type` STATE, which decides which mantle the attribute wears */
#include "core/html/autofill.h"

/* §4.10.19.7.2's own four categories, plus the null the table's first step returns. */
typedef enum { AF_NULL = 0, AF_OFF, AF_AUTOMATIC, AF_NORMAL, AF_CONTACT, AF_CREDENTIAL } AfCategory;

/* §4.10.19.7.2's "determine a field's category" TABLE, in the section's own order and with the section's own
   two columns beside each token: the maximum number of tokens, and the category. Fifty-seven rows, and every
   one of them is load-bearing — the maximum is what step 8 rejects an over-long attribute by, and it is 3 for
   the Normal rows, 4 for the Contact ones and 5 for the single Credential one, so a table that carried the
   category alone would accept `section-a billing home tel` on a Normal field. */
static const struct { const char *token; int max; AfCategory cat; } AF_TOKENS[] = {
    { "off", 1, AF_OFF }, { "on", 1, AF_AUTOMATIC },
    { "name", 3, AF_NORMAL }, { "honorific-prefix", 3, AF_NORMAL }, { "given-name", 3, AF_NORMAL },
    { "additional-name", 3, AF_NORMAL }, { "family-name", 3, AF_NORMAL },
    { "honorific-suffix", 3, AF_NORMAL }, { "nickname", 3, AF_NORMAL },
    { "organization-title", 3, AF_NORMAL }, { "username", 3, AF_NORMAL },
    { "new-password", 3, AF_NORMAL }, { "current-password", 3, AF_NORMAL },
    { "one-time-code", 3, AF_NORMAL }, { "organization", 3, AF_NORMAL },
    { "street-address", 3, AF_NORMAL }, { "address-line1", 3, AF_NORMAL },
    { "address-line2", 3, AF_NORMAL }, { "address-line3", 3, AF_NORMAL },
    { "address-level4", 3, AF_NORMAL }, { "address-level3", 3, AF_NORMAL },
    { "address-level2", 3, AF_NORMAL }, { "address-level1", 3, AF_NORMAL },
    { "country", 3, AF_NORMAL }, { "country-name", 3, AF_NORMAL }, { "postal-code", 3, AF_NORMAL },
    { "cc-name", 3, AF_NORMAL }, { "cc-given-name", 3, AF_NORMAL },
    { "cc-additional-name", 3, AF_NORMAL }, { "cc-family-name", 3, AF_NORMAL },
    { "cc-number", 3, AF_NORMAL }, { "cc-exp", 3, AF_NORMAL }, { "cc-exp-month", 3, AF_NORMAL },
    { "cc-exp-year", 3, AF_NORMAL }, { "cc-csc", 3, AF_NORMAL }, { "cc-type", 3, AF_NORMAL },
    { "transaction-currency", 3, AF_NORMAL }, { "transaction-amount", 3, AF_NORMAL },
    { "language", 3, AF_NORMAL }, { "bday", 3, AF_NORMAL }, { "bday-day", 3, AF_NORMAL },
    { "bday-month", 3, AF_NORMAL }, { "bday-year", 3, AF_NORMAL }, { "sex", 3, AF_NORMAL },
    { "url", 3, AF_NORMAL }, { "photo", 3, AF_NORMAL },
    { "tel", 4, AF_CONTACT }, { "tel-country-code", 4, AF_CONTACT }, { "tel-national", 4, AF_CONTACT },
    { "tel-area-code", 4, AF_CONTACT }, { "tel-local", 4, AF_CONTACT },
    { "tel-local-prefix", 4, AF_CONTACT }, { "tel-local-suffix", 4, AF_CONTACT },
    { "tel-extension", 4, AF_CONTACT }, { "email", 4, AF_CONTACT }, { "impp", 4, AF_CONTACT },
    { "webauthn", 5, AF_CREDENTIAL },
};
#define AF_TOKENS_N ((int)(sizeof(AF_TOKENS) / sizeof(AF_TOKENS[0])))

/* Step 19's list, in the section's own order — the CONTACT tokens a Contact field accepts in front of it. */
static const char *const AF_CONTACT_TOKENS[] = { "home", "work", "mobile", "fax", "pager", NULL };
/* Step 20's list. */
static const char *const AF_MODE_TOKENS[] = { "shipping", "billing", NULL };

/* Infra's ASCII case-insensitive match over a slice that is NOT NUL-terminated. It folds by BYTE and never by
   `strcasecmp`, whose folding is the current LOCALE's — a Turkish locale does not fold `TEL` to `tel`, and
   every comparison in this algorithm is stated as ASCII case-insensitive. */
static bool af_ci_eq(const char *a, size_t alen, const char *lower)
{
    size_t i, n = strlen(lower);

    if (alen != n) return false;
    for (i = 0; i < n; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return true;
}

static const char *af_in_list(const char *const *list, const char *tok, size_t len)
{
    int i;

    for (i = 0; list[i]; i++)
        if (af_ci_eq(tok, len, list[i])) return list[i];
    return NULL;
}

/* §4.10.19.7.2's "determine a field's category, given field": "If the field is not an ASCII case-insensitive
   match for one of the tokens given in the first column of the following table, return the pair (null, null)."
   `*max` is written only when a row matches, which is what the null pair means here — a caller reading it after
   an AF_NULL answer would be reading the previous row's number. */
static AfCategory af_category(const char *tok, size_t len, int *max)
{
    int i;

    for (i = 0; i < AF_TOKENS_N; i++)
        if (af_ci_eq(tok, len, AF_TOKENS[i].token)) { *max = AF_TOKENS[i].max; return AF_TOKENS[i].cat; }
    return AF_NULL;
}

/* §4.10.19.7.1 Autofilling form controls: the autocomplete attribute's MANTLE: "On an input element whose type
   attribute is in the Hidden state, the autocomplete attribute wears the autofill anchor mantle. In all other
   cases, it wears the autofill expectation mantle." So the whole test is one state of one element kind, and a
   `select` or `textarea` never wears the anchor mantle. */
static bool af_wears_anchor_mantle(lxb_dom_node_t *n)
{
    return html_form_input_state(n) == INPUT_STATE_HIDDEN;
}

/* Infra's "split on ASCII whitespace" over the attribute's bytes, as slices INTO those bytes — nothing is
   copied, because every use is a comparison or a concatenation the caller performs. Returns the count and
   fills `off`/`len`; a value with more tokens than `cap` is one the algorithm's step 8 rejects anyway (the
   largest maximum in the table is 5), so the cap is a fact about the table rather than a bound on input:
   a run that fills it is reported as a count the caller compares against the maximum and defaults on. */
#define AF_MAX_TOKENS 8
static int af_split(const char *s, size_t len, size_t *off, size_t *tlen)
{
    size_t i = 0;
    int n = 0;

    while (i < len && n < AF_MAX_TOKENS) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r')) i++;
        if (i >= len) break;
        off[n] = i;
        while (i < len && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r')) i++;
        tlen[n] = i - off[n];
        n++;
    }
    /* A value with MORE than AF_MAX_TOKENS tokens still has to be rejected rather than truncated, so the
       remaining whitespace-separated runs are counted without being recorded: step 8 compares this count with
       the table's maximum, and a truncated count could compare EQUAL and let the value through. */
    while (i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r')) i++;
        if (i >= len) break;
        while (i < len && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r')) i++;
        n++;
    }
    return n;
}

/* A growable answer built by PREPENDING, which is the shape every step from 16.7 onward states ("the
   concatenation of the index th token in tokens, a U+0020 SPACE character, and the previous value of IDL
   value"). Held as one buffer with the pieces recorded in reverse and joined once, so the quadratic rebuild a
   literal prepend would perform is not what the algorithm costs. */
typedef struct { const char *p[AF_MAX_TOKENS + 1]; size_t n[AF_MAX_TOKENS + 1]; int count; } AfValue;

static void af_prepend(AfValue *v, const char *p, size_t n)
{
    CHECK(v->count <= AF_MAX_TOKENS,
          "§4.10.19.7.2 prepended more pieces to the IDL-exposed autofill value than the attribute had tokens "
          "— every prepend consumes one token and the split records at most that many");
    v->p[v->count] = p;
    v->n[v->count] = n;
    v->count++;
}

/* Join the recorded pieces LAST-FIRST, which is the order they were prepended in. `lower_last` is step 23's
   "converted to ASCII lowercase", which applies to the SECTION token alone — every other piece is the page's
   own bytes and the algorithm never folds them, which is why `<input autocomplete="SHIPPING TEL">` reads back
   in the case the markup used. */
static JSValue af_join(JSContext *ctx, const AfValue *v, bool lower_last)
{
    size_t total = 0, w = 0;
    char *buf;
    int i;
    JSValue r;

    for (i = 0; i < v->count; i++) total += v->n[i] + 1;
    buf = (char *)malloc(total ? total : 1);
    CHECK(buf != NULL, "§4.10.19.7.2: OOM building the IDL-exposed autofill value");
    for (i = v->count - 1; i >= 0; i--) {
        size_t k;

        if (w) buf[w++] = ' ';
        for (k = 0; k < v->n[i]; k++) {
            char c = v->p[i][k];
            if (lower_last && i == v->count - 1 && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            buf[w++] = c;
        }
    }
    r = JS_NewStringLen(ctx, buf, w);
    free(buf);
    return r;
}

/* §4.10.19.7.2's algorithm, run over ONE element's attribute bytes, answering ONLY the IDL-EXPOSED AUTOFILL
 * VALUE. Step numbers below are the section's own TOP-LEVEL numbers; where a step holds more than one list its
 * sub-items are named in the section's words rather than numbered, because a bare sub-number would name several
 * different steps (step 19 holds the contact-keyword list AND the substeps that run when one matches).
 *
 * THE OTHER FOUR OUTPUTS ARE LOCALS AND ARE NOT STORED, deliberately. The algorithm also computes the autofill
 * field name, hint set, scope and non-autofill credential type; every one of those is read by a USER AGENT
 * FILLING A FORM, which this engine does not do, and a value computed and written where nothing reads it is
 * the write-with-no-reader half of the broken-contract pair. They are computed here only where the algorithm's
 * own control flow needs them (the credential type gates nothing, so it is not even a local), and the day a
 * consumer exists this function grows an out-parameter rather than a second copy of the walk.
 * THE FORM OWNER IS NOT CONSULTED FOR THE SAME REASON: the Default branch's only use of it is to choose the
 * FIELD NAME between "off" and "on", and that branch has already set the IDL-exposed autofill value to the
 * empty string two steps earlier.
 */
static JSValue af_idl_value(JSContext *ctx, lxb_dom_node_t *n, const char *s, size_t len)
{
    size_t off[AF_MAX_TOKENS], tlen[AF_MAX_TOKENS];
    int count, index, max = 0;
    AfCategory cat;
    AfValue v = { { 0 }, { 0 }, 0 };
    const char *match;
    bool section = false;

    /* Steps 2-3: "Let tokens be the result of splitting the attribute's value on ASCII whitespace. If tokens is
       empty, then jump to the step labeled default." (Step 1's no-attribute case is the caller's — it has the
       attribute in hand or it does not.) */
    count = af_split(s, len, off, tlen);
    if (count == 0) return JS_NewStringLen(ctx, "", 0);
    /* Steps 4-6: the LAST token is the field, and its category decides everything after. */
    index = count - 1;
    if (index >= AF_MAX_TOKENS) return JS_NewStringLen(ctx, "", 0);   /* step 8 rejects it below anyway */
    cat = af_category(s + off[index], tlen[index], &max);
    /* Steps 7-8: "If category is null … If the number of tokens in tokens is greater than maximum tokens". */
    if (cat == AF_NULL || count > max) return JS_NewStringLen(ctx, "", 0);
    /* Step 9: "If category is Off or Automatic but the element's autocomplete attribute is wearing the autofill
       anchor mantle, then jump to the step labeled default." */
    if ((cat == AF_OFF || cat == AF_AUTOMATIC) && af_wears_anchor_mantle(n))
        return JS_NewStringLen(ctx, "", 0);
    /* Steps 10-11: the two one-token values answer with the section's own literals rather than with the
       attribute's bytes, which is what makes `<input autocomplete="OFF">` read back "off". */
    if (cat == AF_OFF) return JS_NewString(ctx, "off");
    if (cat == AF_AUTOMATIC) return JS_NewString(ctx, "on");

    /* Step 15: "Let IDL value have the same value as field." */
    af_prepend(&v, s + off[index], tlen[index]);

    /* Step 16: the Credential arm. Its substeps re-read the category from the PRECEDING token, so the checks
       that follow run against that one and not against `webauthn`. */
    if (cat == AF_CREDENTIAL && af_ci_eq(s + off[index], tlen[index], "webauthn")) {
        if (index == 0) return af_join(ctx, &v, false);           /* "skip to the step labeled done" */
        index--;
        cat = af_category(s + off[index], tlen[index], &max);
        if (cat != AF_NORMAL && cat != AF_CONTACT) return JS_NewStringLen(ctx, "", 0);
        /* "If index is greater than maximum tokens minus one (i.e. if the number of remaining tokens is greater
           than maximum tokens)" — the parenthesis is the section's own gloss and is why this is `>` on a
           zero-based index rather than the token COUNT step 8 compared. */
        if (index > max - 1) return JS_NewStringLen(ctx, "", 0);
        af_prepend(&v, s + off[index], tlen[index]);
    }

    /* Steps 17-18 are TOP-LEVEL and shared by both paths above: stop if the cursor is already at the first
       token, otherwise move to the one before it. Reading them as substeps of step 16 is the mistake this
       algorithm invites — it would make `<input autocomplete="home tel">` test "tel" against the contact list
       and answer "" where a browser answers "home tel". */
    if (index == 0) return af_join(ctx, &v, false);
    index--;

    /* Step 19: a Contact field may be preceded by one of the section's five contact keywords.
       WHAT IS PREPENDED IS THE LIST'S STRING AND NOT THE ATTRIBUTE'S BYTES — "Let contact be the matching
       string from the list above … Let IDL value be the concatenation of contact, a U+0020 SPACE character,
       and the previous value of IDL value." The match is ASCII case-insensitive and the thing joined is the
       lowercase spelling, so `<input autocomplete="HOME tel">` reads back `home tel`. That is the opposite of
       what steps 15 and 16 do with the page's own tokens, which is why the two are written differently here:
       `field` keeps the case the markup used and these two prefixes do not. */
    if (cat == AF_CONTACT && (match = af_in_list(AF_CONTACT_TOKENS, s + off[index], tlen[index])) != NULL) {
        af_prepend(&v, match, strlen(match));
        if (index == 0) return af_join(ctx, &v, false);
        index--;
    }
    /* Step 20: and either kind may be preceded by a mode — the list's string again, for the same reason. */
    if ((match = af_in_list(AF_MODE_TOKENS, s + off[index], tlen[index])) != NULL) {
        af_prepend(&v, match, strlen(match));
        if (index == 0) return af_join(ctx, &v, false);
        index--;
    }
    /* Steps 21-25: what is left must be exactly one `section-` token, and it is the ONE piece the algorithm
       lowercases. */
    if (index != 0) return JS_NewStringLen(ctx, "", 0);
    if (tlen[index] < 8 || !af_ci_eq(s + off[index], 8, "section-")) return JS_NewStringLen(ctx, "", 0);
    af_prepend(&v, s + off[index], tlen[index]);
    section = true;
    return af_join(ctx, &v, section);
}

/* WEB IDL §3.7.6 Attributes' receiver check, a THROW and not an assert. The member is on three prototypes and
   §4.10.19.7.2 states the value over "each input element to which the autocomplete attribute applies, each
   select element, and each textarea element", so those three tags in the HTML namespace are the brand. */
static lxb_dom_node_t *af_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    size_t len = 0;
    const lxb_char_t *local;

    if (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT && n->ns == LXB_NS_HTML) {
        local = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
        if (local && (af_ci_eq((const char *)local, len, "input") ||
                      af_ci_eq((const char *)local, len, "select") ||
                      af_ci_eq((const char *)local, len, "textarea")))
            return n;
    }
    JS_ThrowTypeError(ctx, "the `autocomplete` IDL attribute was reached on an element that does not have it");
    return NULL;
}

/* §4.10.19.7.2 Processing model: "The autocomplete IDL attribute, on getting, must return the element's
 * IDL-exposed autofill value."
 *
 * A CONCOLIC ATTRIBUTE VALUE STAYS CONCOLIC, the shape core/dom/element.c's element_reflect_url_get uses: the
 * REAL algorithm runs on the concrete EXAMPLE and the answer is DERIVED from it. That matters here rather than
 * being uniformity for its own sake — the algorithm COMPOSES the page's own tokens into its answer, and the
 * `section-` one is nearly free text, so `<input autocomplete="section-{hash} tel">` reads back bytes an
 * attacker chose. Answering with a bare string would de-taint the one member that carries them out. */
static JSValue js_autofill_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = af_receiver(ctx, this_val);
    JSValue raw, concrete, out;
    const char *s;
    size_t len = 0;

    (void)magic;
    if (!n) return JS_EXCEPTION;
    /* Step 1: "If the element has no autocomplete attribute, then jump to the step labeled default", whose
       IDL-exposed autofill value is the empty string. */
    raw = element_attr_get_value(ctx, this_val, "autocomplete");
    if (JS_IsException(raw)) return raw;
    if (JS_IsNull(raw)) { JS_FreeValue(ctx, raw); return JS_NewStringLen(ctx, "", 0); }

    concrete = concolic_is(raw) ? concolic_example(ctx, raw) : JS_DupValue(ctx, raw);
    /* A concolic with no example yet has no bytes to split, and §4.10.19.7.2 has no step that invents them —
       so the answer is the value itself, still carrying its provenance, exactly as the URL model answers for
       an attribute it cannot parse. */
    if (!JS_IsString(concrete)) { JS_FreeValue(ctx, concrete); return raw; }
    s = JS_ToCStringLen(ctx, &len, concrete);
    JS_FreeValue(ctx, concrete);
    if (!s) { JS_FreeValue(ctx, raw); return JS_EXCEPTION; }

    out = af_idl_value(ctx, n, s, len);
    JS_FreeCString(ctx, s);
    if (concolic_is(raw)) out = concolic_builtin_hook(ctx, raw, "autocomplete", out);   /* consumes `out` */
    JS_FreeValue(ctx, raw);
    return out;
}

/* §2.6.2's `[ReflectSetter]`: the setter reflects and writes the content attribute. */
static JSValue js_autofill_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (!af_receiver(ctx, this_val)) return JS_EXCEPTION;
    element_attr_set_value(ctx, this_val, "autocomplete", val);
    return JS_UNDEFINED;
}

/* Declared once per AGENT; installed once per realm on each of the three prototypes. */
static int g_id_set_autocomplete = -1;

void autofill_init(JSContext *ctx)
{
    DCHECK(g_id_set_autocomplete < 0, "autofill_init ran twice in one runtime — the setter is declared once "
                                      "per AGENT and installed once per realm");
    g_id_set_autocomplete = idl_setter_id(ctx, IDL_DOMSTRING, false, js_autofill_set, 0);
}

/* The agent-lifetime release. The only agent state is the setter's POOL ID — an int, so there is nothing to
   hand back — reset for the reason core/html/html_base_element.c resets its one. */
void autofill_free(void)
{
    g_id_set_autocomplete = -1;
}

void autofill_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_set_autocomplete >= 0,
           "§4.10.19.7's `autocomplete` was installed before autofill_init declared its setter");
    idl_install_accessor(ctx, proto, "autocomplete", js_autofill_get, 0, g_id_set_autocomplete);
}
