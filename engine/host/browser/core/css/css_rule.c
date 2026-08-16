/* CSSOM §6.4.2 CSSRule and §6.4.3 CSSStyleRule. See css_rule.h for why a rule is made of text.
 *
 * THE RECORD TIME-TRAVELS BECAUSE §6.4.3's `selectorText` IS A SETTER. It is written into a C record behind a
 * class opaque where no property hook can see it, so one arm of a fork retargeting a rule would have retargeted
 * it for its sibling and for every flow the frontier resumes afterwards. The capture is in the ACCESSOR every
 * member goes through, so a record a flow has REACHED is one it may write and there is no write site to miss.
 * RULE_VALS is the same list rule_finalizer frees and rule_gc_mark marks — read the three together.
 *
 * §6.4.2's `type` IS NOT STORED. It is "return 1 if the object is a CSSStyleRule, 3 if CSSImportRule, ..." — a
 * question about WHICH INTERFACE this is, and this build has exactly one, so the answer comes from the class
 * rather than from a field a creator could set inconsistently with the prototype it handed out. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/css/css.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_rule.h"
#include "core/css/css_style_declaration.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/cow.h"

typedef struct CssRuleData {
    JSValue parent_style_sheet;  /* §6.4.2 "parent CSS style sheet" (OWNED) */
    JSValue parent_rule;         /* §6.4.2 "parent CSS rule" (OWNED) */
    JSValue selector_text;       /* §6.4.3's associated selector list, serialized (OWNED) */
    JSValue block_text;          /* the rule's associated declarations, serialized (OWNED) — see the header */
    /* §6.4.3's `[SameObject] style` — the CSSStyleProperties over `block_text`, minted once because a page
       holds `rule.style` and compares it. JS_UNDEFINED until something asks. (OWNED) */
    JSValue style;
} CssRuleData;

static JSClassID g_rule_class;
static int       g_cssrule_proto_slot = -1;   /* CSSRule.prototype, per realm */
static int       g_id_set_selector = -1;

static const uint16_t RULE_VALS[] = {
    (uint16_t)offsetof(CssRuleData, parent_style_sheet),
    (uint16_t)offsetof(CssRuleData, parent_rule),
    (uint16_t)offsetof(CssRuleData, selector_text),
    (uint16_t)offsetof(CssRuleData, block_text),
    (uint16_t)offsetof(CssRuleData, style),
};
static const CowRecord RULE_REC = { sizeof(CssRuleData), RULE_VALS, 5 };

/* THE ACCESSOR, AND THEREFORE THE CAPTURE POINT. */
static CssRuleData *rule_of(JSValueConst v)
{
    CssRuleData *r = JS_GetOpaque(v, g_rule_class);

    if (r) cow_capture_host_record(v, r, &RULE_REC);
    return r;
}

static CssRuleData *rule_here(JSContext *ctx, JSValueConst v)
{
    CssRuleData *r = rule_of(v);

    if (!r) {
        JS_ThrowTypeError(ctx, "not a CSSRule");
        return NULL;
    }
    return r;
}

bool css_rule_is(JSValueConst v)
{
    DCHECK(g_rule_class != 0, "a value was asked whether it is a CSSRule before the interface existed");
    return JS_GetOpaque(v, g_rule_class) != NULL;
}

/* Through JS_GetOpaque, never the accessor: a capture during collection would dup values on an object being
   torn down. */
static void rule_finalizer(JSRuntime *rt, JSValue val)
{
    CssRuleData *r = JS_GetOpaque(val, g_rule_class);

    if (!r) return;
    JS_FreeValueRT(rt, r->parent_style_sheet);
    JS_FreeValueRT(rt, r->parent_rule);
    JS_FreeValueRT(rt, r->selector_text);
    JS_FreeValueRT(rt, r->block_text);
    JS_FreeValueRT(rt, r->style);
    free(r);
}

static void rule_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    CssRuleData *r = JS_GetOpaque(val, g_rule_class);

    if (!r) return;
    JS_MarkValue(rt, r->parent_style_sheet, mark_func);
    JS_MarkValue(rt, r->parent_rule, mark_func);
    JS_MarkValue(rt, r->selector_text, mark_func);
    JS_MarkValue(rt, r->block_text, mark_func);
    JS_MarkValue(rt, r->style, mark_func);
}

JSValue css_style_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                           const char *selector_text, const char *block_text)
{
    JSValue proto, obj;
    CssRuleData *r;

    DCHECK(g_rule_class != 0, "a CSSStyleRule was built before css_rule_init declared the interface");
    DCHECK(selector_text != NULL && block_text != NULL,
           "a CSSStyleRule was built without both of the texts it IS — a rule with no selector matches nothing "
           "and a rule with no body is the lossy shape css_rule.h exists to refuse");
    proto = JS_GetClassProto(ctx, g_rule_class);
    DCHECK(!JS_IsNull(proto), "a CSSStyleRule was built in a realm with no CSSStyleRule.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_rule_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    r = calloc(1, sizeof(*r));
    CHECK(r != NULL, "the CSSStyleRule record allocation failed");
    r->parent_style_sheet = JS_DupValue(ctx, parent_style_sheet);
    r->parent_rule = JS_DupValue(ctx, parent_rule);
    r->selector_text = JS_NewString(ctx, selector_text);
    r->block_text = JS_NewString(ctx, block_text);
    /* A calloc'd JSValue is not JS_UNDEFINED — its tag is whatever zero means — so the one field no creator
       supplies is placed EXPLICITLY, like every other value the record owns. */
    r->style = JS_UNDEFINED;
    JS_SetOpaque(obj, r);
    realm_awaits(ctx, "CSSGroupingRule",
                 "CSSOM §6.4.3 declares `CSSStyleRule : CSSGroupingRule`, and css_rule_install_proto chains "
                 "CSSStyleRule.prototype straight to CSSRule.prototype because §6.4.5 was not built. It is "
                 "now: put CSSGroupingRule.prototype between the two, so a style rule inherits the `cssRules`, "
                 "`insertRule` and `deleteRule` that make its NESTED rules reachable");
    return obj;
}

void css_rule_orphan(JSContext *ctx, JSValueConst rule)
{
    /* THE BRAND IS ASSERTED, NOT THROWN: this is an algorithm §6.4 invokes on a rule it already holds, never a
       member a page can apply to a stranger, and a TypeError here would leave a pending exception in a C
       caller with no member to return it from. */
    CssRuleData *r = rule_of(rule);

    DCHECK(r != NULL, "§6.4's remove a CSS rule was invoked on something that is not a CSS rule");
    JS_FreeValue(ctx, r->parent_style_sheet);
    JS_FreeValue(ctx, r->parent_rule);
    r->parent_style_sheet = JS_NULL;
    r->parent_rule = JS_NULL;
}

/* ---- §6.4.2's members ------------------------------------------------------------------------------------ */

enum { CR_PARENT_RULE = 0, CR_PARENT_STYLE_SHEET, CR_TYPE, CR_SELECTOR_TEXT };

static JSValue js_rule_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssRuleData *r = rule_here(ctx, this_val);

    if (!r) return JS_EXCEPTION;
    switch (magic) {
    /* "The parentRule attribute must return the parent CSS rule." Null wherever no rule encloses this one,
       which is every rule in this build until CSSGroupingRule nests one. */
    case CR_PARENT_RULE: return JS_DupValue(ctx, r->parent_rule);
    /* "The parentStyleSheet attribute must return the parent CSS style sheet." The spec's own note is the
       whole of when it is null: "the only circumstance where null is returned when a rule has been removed." */
    case CR_PARENT_STYLE_SHEET: return JS_DupValue(ctx, r->parent_style_sheet);
    case CR_TYPE:
        /* §6.4.2's deprecated `type`: "If the object is a CSSStyleRule, return 1." There is exactly one rule
           interface in this build, so this is that answer and not a stored field — and the enumeration is
           FROZEN by the spec ("no new values will be added"), so a rule interface that lands later brings its
           own constant with it rather than needing a number invented here. */
        return JS_NewUint32(ctx, 1);
    default:
        DCHECK(magic == CR_SELECTOR_TEXT, "a CSS rule attribute ran with a magic §6.4 does not declare");
        /* §6.4.3: "on getting, must return the result of serializing the rule's associated selector list" —
           which is what the parse handed over and what the setter below replaces. */
        return JS_DupValue(ctx, r->selector_text);
    }
}

/* The one rule a selector probe keeps: the SERIALIZED selector list lexbor accepted, which is the canonical
   form §6.4.3's getter must answer afterwards. A parse that produced no style rule leaves `*out` NULL, which is
   the setter's "the algorithm returned null, do nothing". */
static void css_rule_selector_probe(void *ud, unsigned type, const char *sel, const char *block)
{
    char **out = ud;

    (void)block;
    if (type != (unsigned)LXB_CSS_RULE_STYLE || *out || !sel || !*sel) return;
    *out = strdup(sel);
    CHECK(*out != NULL, "cssom: OOM keeping a parsed selector list");
}

/* §6.4.3's setter: "Run the parse a group of selectors algorithm on the given value. If the algorithm returns a
   non-null value replace the associated selector list with the returned value. Otherwise, if the algorithm
   returns a null value, DO NOTHING." An invalid selector is silently ignored — not a throw, and not a stored
   invalid string, which is why the value goes back through the parser rather than into the slot. */
static JSValue js_rule_set_selector(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    CssRuleData *r = rule_here(ctx, this_val);
    const char *v;
    char *reserialized = NULL;
    unsigned n;

    (void)magic;
    if (!r) return JS_EXCEPTION;
    v = JS_ToCString(ctx, val);   /* a real string by now: the declaration converted it */
    if (!v) return JS_EXCEPTION;
    /* THE GROUP OF SELECTORS IS PARSED BY PARSING A RULE WITH AN EMPTY BODY, because that is the one entry the
       agent's parser exposes and because it answers exactly the question §6.4.3 asks: a value lexbor accepts as
       a selector list comes back as one style rule whose serialization is the canonical form the getter must
       then return, and a value it rejects produces no style rule at all. */
    {
        char *probe;
        size_t vlen = strlen(v);

        probe = malloc(vlen + 4);
        CHECK(probe != NULL, "cssom: OOM parsing a selector list");
        memcpy(probe, v, vlen);
        memcpy(probe + vlen, "{}", 3);
        n = cssom_parse_rules(probe, vlen + 2, css_rule_selector_probe, &reserialized);
        free(probe);
    }
    JS_FreeCString(ctx, v);
    if (n == 1 && reserialized) {
        JS_FreeValue(ctx, r->selector_text);
        r->selector_text = JS_NewString(ctx, reserialized);
    }
    free(reserialized);
    return JS_UNDEFINED;
}

/* ---- §6.4.3's `style`, and the DECLARATIONS behind it ----------------------------------------------------- */

/* §6.4.3: "The style attribute must return a CSSStyleProperties object for the style rule, with the following
   properties: computed flag unset, readonly flag unset, declarations THE DECLARED DECLARATIONS IN THE RULE,
   parent CSS rule THIS, owner node null." [SameObject], so the block is remembered on the record — a page holds
   `rule.style` and compares it, and a fresh object per read makes every such comparison false.
   IT IS MINTED THROUGH THE CAPTURING ACCESSOR, which is what makes the memo per-flow: the block belongs to the
   flow that first asked for it, and a sibling that asks gets its own over the same declarations. */
static JSValue js_rule_style(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssRuleData *r = rule_here(ctx, this_val);

    (void)magic;
    if (!r) return JS_EXCEPTION;
    if (!JS_IsObject(r->style)) {
        JS_FreeValue(ctx, r->style);
        r->style = cssom_style_properties_for_rule(ctx, this_val);
    }
    return JS_DupValue(ctx, r->style);
}

/* THE RULE'S DECLARATIONS, as the text they are. §6.6's block reads them through here and writes them back
   through the setter below, so the two components share ONE storage rather than each keeping a copy that could
   disagree — which is the same reason an element's block is the `style` attribute and not a parsed cache.
   OWNED: the caller frees. NULL for a rule whose body is empty. */
char *css_rule_block_text(JSContext *ctx, JSValueConst rule, size_t *plen)
{
    CssRuleData *r = rule_of(rule);
    const char *c;
    size_t len = 0;
    char *out;

    DCHECK(r != NULL, "a rule's declaration block was read off something that is not a CSS rule");
    DCHECK(plen != NULL, "a rule's declaration block was read with nowhere to report its length");
    *plen = 0;
    c = JS_ToCStringLen(ctx, &len, r->block_text);
    if (!c) return NULL;                        /* the conversion threw; the caller's read answers empty */
    if (len == 0) { JS_FreeCString(ctx, c); return NULL; }
    out = malloc(len + 1);
    CHECK(out != NULL, "cssom: OOM copying a rule's declaration block");
    memcpy(out, c, len);
    out[len] = '\0';
    JS_FreeCString(ctx, c);
    *plen = len;
    return out;
}

/* The write half. It goes through `rule_of`, so the per-flow COW delta has already taken a copy of the record
   and one arm's `rule.style.color = 'red'` is invisible to its sibling and to every flow the frontier resumes
   afterwards — the same guarantee `selectorText`'s setter has, for the same reason and at the same site. */
void css_rule_set_block_text(JSContext *ctx, JSValueConst rule, const char *text, size_t len)
{
    CssRuleData *r = rule_of(rule);

    DCHECK(r != NULL, "a rule's declaration block was written on something that is not a CSS rule");
    DCHECK(text != NULL, "a rule's declaration block was written with no text — an emptied block is the empty "
                         "string, which is what serializing no declarations produces");
    JS_FreeValue(ctx, r->block_text);
    r->block_text = JS_NewStringLen(ctx, text, len);
}

/* ---- the interfaces -------------------------------------------------------------------------------------- */

/* §6.4.2's historical constants, which the IDL declares on the interface AND its prototype. */
static const struct { const char *name; uint32_t v; } CR_CONSTS[] = {
    { "STYLE_RULE", 1 }, { "CHARSET_RULE", 2 }, { "IMPORT_RULE", 3 }, { "MEDIA_RULE", 4 },
    { "FONT_FACE_RULE", 5 }, { "PAGE_RULE", 6 }, { "MARGIN_RULE", 9 }, { "NAMESPACE_RULE", 10 },
};

static void rule_install_constants(JSContext *ctx, JSValueConst target)
{
    unsigned i;

    for (i = 0; i < sizeof(CR_CONSTS) / sizeof(CR_CONSTS[0]); i++)
        JS_DefinePropertyValueStr(ctx, (JSValue)target, CR_CONSTS[i].name,
                                  JS_NewUint32(ctx, CR_CONSTS[i].v), JS_PROP_ENUMERABLE);
}

void css_rule_init(JSContext *ctx)
{
    JSClassDef d = { "CSSStyleRule", rule_finalizer, rule_gc_mark };

    if (g_rule_class) return;   /* one AGENT, one class and one set of pool entries */
    JS_NewClassID(JS_GetRuntime(ctx), &g_rule_class);
    JS_NewClass(JS_GetRuntime(ctx), g_rule_class, &d);
    g_cssrule_proto_slot = realm_value_declare(ctx, "CSSOM §6.4.2 CSSRule.prototype");
    g_id_set_selector = idl_setter_id(ctx, IDL_DOMSTRING, false, js_rule_set_selector, 0);
    realm_declare_intrinsic(css_rule_install_proto);
}

void css_rule_install_proto(JSContext *ctx)
{
    JSValue base, proto, prev;

    DCHECK(g_rule_class != 0, "a realm asked for the rule prototypes before the interfaces existed");
    prev = JS_GetClassProto(ctx, g_rule_class);
    DCHECK(JS_IsNull(prev), "css_rule_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* §6.4.2's CSSRule.prototype. Nothing is an instance of it — §6.4.2 is "an abstract, base CSS rule" — so it
       holds no class of its own, like §6.1.1's StyleSheet.prototype. */
    base = JS_NewObject(ctx);
    CHECK(!JS_IsException(base), "CSSRule.prototype could not be allocated");
    idl_interface_tag(ctx, base, "CSSRule");
    idl_install_accessor(ctx, base, "parentRule", js_rule_get, CR_PARENT_RULE, -1);
    idl_install_accessor(ctx, base, "parentStyleSheet", js_rule_get, CR_PARENT_STYLE_SHEET, -1);
    idl_install_accessor(ctx, base, "type", js_rule_get, CR_TYPE, -1);
    rule_install_constants(ctx, base);

    /* §6.4.3 declares `CSSStyleRule : CSSGroupingRule`, and this chains to CSSRule.prototype because
       CSSGroupingRule is not built — it is NESTED RULES, with a `cssRules`, an `insertRule` and a `deleteRule`
       of its own over them. The chain is therefore one link short; css_style_rule_new asserts against that
       link's arrival, because a realm's GLOBALS do not exist yet at this line and a probe here could never
       fire however long CSSGroupingRule had been built. */
    proto = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(proto), "CSSStyleRule.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CSSStyleRule");
    idl_install_accessor(ctx, proto, "selectorText", js_rule_get, CR_SELECTOR_TEXT, g_id_set_selector);
    idl_install_accessor(ctx, proto, "style", js_rule_style, 0, cssom_put_forwards_setter());
    JS_SetClassProto(ctx, g_rule_class, proto);
    realm_value_set(ctx, g_cssrule_proto_slot, base);
}

void css_rule_install(JSContext *ctx, JSValueConst global)
{
    JSValue base = realm_value_get(ctx, g_cssrule_proto_slot);
    JSValue proto = JS_GetClassProto(ctx, g_rule_class);

    DCHECK(!JS_IsNull(proto) && JS_IsObject(base),
           "the rule interfaces were installed in a realm that never ran their prototype install");
    /* §6.4.2's constants are on the INTERFACE OBJECT as well as the prototype, which is what Web IDL says of a
       `const` and what `CSSRule.STYLE_RULE` reads. */
    {
        JSValue iface = idl_interface_object(ctx, "CSSRule", base);
        rule_install_constants(ctx, iface);
        JS_SetPropertyStr(ctx, (JSValue)global, "CSSRule", iface);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "CSSStyleRule", idl_interface_object(ctx, "CSSStyleRule", proto));
    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, proto);
}

void css_rule_free(JSContext *ctx)
{
    (void)ctx;   /* both prototypes are the REALM's — released with its context */
}
