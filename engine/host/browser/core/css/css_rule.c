/* CSSOM §6.4's CSS rules and CSS Conditional §7.2/§7.3's conditional group rule. See css_rule.h for why a rule
 * is made of text, why a grouping rule's child list is a JS Array, and why one class carries every interface.
 *
 * THE RECORD TIME-TRAVELS BECAUSE ALMOST EVERYTHING ON IT IS SETTABLE. §6.4.3's `selectorText` is a setter,
 * §6.4.3's `style` writes the declaration block back through this record, §6.4.5's `insertRule`/`deleteRule`
 * mutate the child list and §7.3's `media` is `[PutForwards=mediaText]`. Every one of those lands in a C record
 * behind a class opaque where no property hook can see it, so one arm of a fork retargeting a rule would have
 * retargeted it for its sibling and for every flow the frontier resumes afterwards. The capture is in the
 * ACCESSOR every member goes through, so a record a flow has REACHED is one it may write and there is no write
 * site to miss. RULE_VALS is the same list rule_finalizer frees and rule_gc_mark marks — read the three
 * together, because a field added to one and not the others is exactly the bug the layout exists to prevent. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_at_rule_prelude.h"
#include "core/css/css_rule.h"
#include "core/css/css_rule_list.h"
#include "core/css/css_serialize.h"
#include "core/css/css_style_declaration.h"
#include "core/css/media_list.h"
#include "core/css/media_query.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/cow.h"

/* §6.4's TYPE state item, which IS which interface this rule is — the values are §6.4.2's own constants, so
   there is one number rather than a stored type beside an interface tag that could disagree with it. */
enum { RULE_TYPE_STYLE = 1, RULE_TYPE_IMPORT = 3, RULE_TYPE_MEDIA = 4, RULE_TYPE_FONT_FACE = 5,
       RULE_TYPE_NAMESPACE = 10 };

/* WHERE A RULE MAY SIT IN A STYLE SHEET, as the one number the ordering CSS specifies is stated over. CSS
   Cascade §2: "any @import rules must precede all other valid at-rules and style rules in a style sheet";
   CSS Namespaces §2: "any @namespace rules must follow all @charset and @import rules and precede all other
   non-ignored at-rules and style rules". So a sheet's rules are RANK-SORTED, and §6.4 step 5's "cannot be
   inserted ... due to constraints specified by CSS" is exactly "the insertion would break that sort". Stating
   it as a rank rather than as a pair of special cases is what makes both directions fall out of one test: a
   style rule BEFORE an `@import` is refused (css/cssom/insertRule-import-no-index.html) by the same line that
   refuses an `@import` after one. */
enum { RANK_IMPORT = 0, RANK_NAMESPACE = 1, RANK_OTHER = 2 };

typedef struct CssRuleData {
    JSValue parent_style_sheet;  /* §6.4.2 "parent CSS style sheet" (OWNED) */
    JSValue parent_rule;         /* §6.4.2 "parent CSS rule" (OWNED) */
    JSValue selector_text;       /* §6.4.3's selector list, serialized — JS_NULL on a rule that has none */
    JSValue block_text;          /* the rule's declarations, serialized — JS_NULL on a rule that has none */
    /* §6.4.3's `[SameObject] style` — the CSSStyleProperties over `block_text`, minted once because a page
       holds `rule.style` and compares it. JS_UNDEFINED until something asks. (OWNED) */
    JSValue style;
    /* §6.4's "CHILD CSS RULES" — an Array on a §6.4.5 GROUPING rule (`CSSStyleRule : CSSGroupingRule` and
       `CSSMediaRule : CSSConditionRule : CSSGroupingRule`), and JS_NULL on one that contains no rules at all.
       See css_rule.h for why it is an Array and not a lexbor rule list. (OWNED) */
    JSValue child_rules;
    /* §6.4.5's `[SameObject] readonly attribute CSSRuleList cssRules` over it, remembered for the same reason
       `style` is, and SHARING that very Array, which is what its liveness IS. (OWNED) */
    JSValue rule_list;
    /* §7.3's `[SameObject] media` — a §4.4 MediaList. §6.4.4's `media` is the same field: "the value of the
       media attribute of the associated CSS style sheet", which for an `@import` is the media query list the
       at-rule itself declared and is what CSS Cascade §2 says that sheet's media IS. JS_NULL on a rule that
       has neither. (OWNED) */
    JSValue media;
    /* §6.4.4's three remaining texts, each JS_NULL on a rule that is not an `@import` — and `layer_name` and
       `supports_text` are JS_NULL on one that declares no layer and no supports condition, which is the
       attribute's own null and not an absence this record has to distinguish from it. (OWNED) */
    JSValue href;
    JSValue layer_name;
    JSValue supports_text;
    /* §6.4.9's two, JS_NULL on a rule that is not an `@namespace`. `prefix` is the EMPTY STRING for the
       default namespace — "the prefix ... or the empty string if there is no prefix" — so a JS_NULL here is
       only ever "this is not a namespace rule". (OWNED) */
    JSValue namespace_uri;
    JSValue prefix;
    uint16_t type;
} CssRuleData;

static JSClassID g_rule_class;
/* THE INTERFACE PROTOTYPES, each the REALM's — §3.7, and core/realm.h's slot store IS quickjs's own per-context
   slot array, freed with the context. Three of them are abstract (nothing is an instance of CSSRule,
   CSSGroupingRule or CSSConditionRule) and the rest are concrete; all are held the same way because a rule is
   built with an EXPLICIT prototype chosen from its type, so the class's own proto slot decides nothing. */
enum { PROTO_RULE = 0, PROTO_GROUPING, PROTO_STYLE, PROTO_CONDITION, PROTO_MEDIA, PROTO_IMPORT,
       PROTO_NAMESPACE, PROTO_FONT_FACE, PROTO_N };
static int g_proto_slot[PROTO_N];
static int g_id_set_selector = -1, g_id_set_css_text = -1, g_id_insert_rule = -1, g_id_delete_rule = -1;

static const uint16_t RULE_VALS[] = {
    (uint16_t)offsetof(CssRuleData, parent_style_sheet),
    (uint16_t)offsetof(CssRuleData, parent_rule),
    (uint16_t)offsetof(CssRuleData, selector_text),
    (uint16_t)offsetof(CssRuleData, block_text),
    (uint16_t)offsetof(CssRuleData, style),
    (uint16_t)offsetof(CssRuleData, child_rules),
    (uint16_t)offsetof(CssRuleData, rule_list),
    (uint16_t)offsetof(CssRuleData, media),
    (uint16_t)offsetof(CssRuleData, href),
    (uint16_t)offsetof(CssRuleData, layer_name),
    (uint16_t)offsetof(CssRuleData, supports_text),
    (uint16_t)offsetof(CssRuleData, namespace_uri),
    (uint16_t)offsetof(CssRuleData, prefix),
};
static const CowRecord RULE_REC = { sizeof(CssRuleData), RULE_VALS,
                                    (int)(sizeof(RULE_VALS) / sizeof(RULE_VALS[0])) };

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

/* Web IDL §3.7.5's brand for a member declared on a DERIVED interface's prototype: a page can read
   `CSSMediaRule.prototype.media` and apply it to a style rule, and the answer is a TypeError rather than a read
   of a JS_NULL slot. The interface a rule IS is its stored type — see css_rule.h. */
static CssRuleData *rule_here_typed(JSContext *ctx, JSValueConst v, uint16_t type, const char *iface)
{
    CssRuleData *r = rule_of(v);

    if (!r || r->type != type) {
        JS_ThrowTypeError(ctx, "a %s member was reached on something that is not a %s", iface, iface);
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
    JS_FreeValueRT(rt, r->child_rules);
    JS_FreeValueRT(rt, r->rule_list);
    JS_FreeValueRT(rt, r->media);
    JS_FreeValueRT(rt, r->href);
    JS_FreeValueRT(rt, r->layer_name);
    JS_FreeValueRT(rt, r->supports_text);
    JS_FreeValueRT(rt, r->namespace_uri);
    JS_FreeValueRT(rt, r->prefix);
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
    JS_MarkValue(rt, r->child_rules, mark_func);
    JS_MarkValue(rt, r->rule_list, mark_func);
    JS_MarkValue(rt, r->media, mark_func);
    JS_MarkValue(rt, r->href, mark_func);
    JS_MarkValue(rt, r->layer_name, mark_func);
    JS_MarkValue(rt, r->supports_text, mark_func);
    JS_MarkValue(rt, r->namespace_uri, mark_func);
    JS_MarkValue(rt, r->prefix, mark_func);
}

/* ---- §6.4's CSS RULE LIST, as INFRA's list operations over an Array ---------------------------------------- */

static uint32_t rules_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

static void rules_insert_at(JSContext *ctx, JSValueConst list, uint32_t i, JSValue v)   /* CONSUMES v */
{
    uint32_t n = rules_len(ctx, list), k;

    DCHECK(i <= n, "§6.4 inserted a CSS rule at an index past the end of the list");
    for (k = n; k > i; k--)
        JS_SetPropertyUint32(ctx, list, k, JS_GetPropertyUint32(ctx, list, k - 1));
    JS_SetPropertyUint32(ctx, list, i, v);
}

static void rules_remove_at(JSContext *ctx, JSValueConst list, uint32_t i)
{
    uint32_t n = rules_len(ctx, list), k;

    DCHECK(i < n, "§6.4 removed a CSS rule at an index the list does not have");
    for (k = i + 1; k < n; k++)
        JS_SetPropertyUint32(ctx, list, k - 1, JS_GetPropertyUint32(ctx, list, k));
    JS_SetPropertyStr(ctx, list, "length", JS_NewUint32(ctx, n - 1));
}

/* IS THIS RULE TYPE A §6.4.5 GROUPING RULE — "an at-rule that CONTAINS OTHER RULES nested inside itself", plus
   the style rule CSS Nesting made one. It is asked in three places and must be ONE fact: the create decides
   whether to allocate a child list at all, `rule_child_rules` asserts it has one, and §3.7.5's brand check on
   `cssRules`/`insertRule`/`deleteRule` decides whether the member may run on this receiver — a page can reach
   `CSSGroupingRule.prototype.insertRule` and apply it to an `@import` rule, and the answer is a TypeError and
   not an insertion into a list that rule does not have. */
static bool rule_type_is_grouping(uint16_t type)
{
    return type == RULE_TYPE_STYLE || type == RULE_TYPE_MEDIA;
}

/* §6.4.5's CHILD CSS RULES — the very Array a grouping rule's `cssRules` shares. OWNED. */
static JSValue rule_child_rules(JSContext *ctx, JSValueConst rule)
{
    CssRuleData *r = rule_of(rule);

    DCHECK(r != NULL, "a rule's child CSS rules were read off something that is not a CSS rule");
    DCHECK(rule_type_is_grouping(r->type),
           "a rule that is not a §6.4.5 grouping rule was asked for its child CSS rules. An `@import`, an "
           "`@namespace` and an `@font-face` contain no rules at all, so the answer is not an empty list — it "
           "is that the question does not apply, and every member that could ask it is brand-checked");
    DCHECK(JS_IsArray(r->child_rules),
           "a CSS rule's child list is not an Array — the create allocates one for every grouping rule before "
           "it is handed to anybody, and nothing replaces it");
    return JS_DupValue(ctx, r->child_rules);
}

/* The receiver of a §6.4.5 member, brand-checked against the interface that DECLARES it. */
static CssRuleData *rule_here_grouping(JSContext *ctx, JSValueConst v)
{
    CssRuleData *r = rule_of(v);

    if (!r || !rule_type_is_grouping(r->type)) {
        JS_ThrowTypeError(ctx, "a CSSGroupingRule member was reached on something that is not a grouping rule");
        return NULL;
    }
    return r;
}

/* ---- creating a rule --------------------------------------------------------------------------------------- */

/* The fields EVERY rule has. The record is COMPLETE at JS_SetOpaque — every owned value is placed, including
   the ones this rule type does not use — so a half-built rule cannot exist for a finalizer to meet.
   `proto_slot` picks the interface prototype out of this realm's set. */
static JSValue rule_new(JSContext *ctx, int proto_slot, uint16_t type, JSValueConst parent_style_sheet,
                        JSValueConst parent_rule)
{
    JSValue proto, obj;
    CssRuleData *r;

    DCHECK(g_rule_class != 0, "a CSS rule was built before css_rule_init declared the interfaces");
    proto = realm_value_get(ctx, g_proto_slot[proto_slot]);
    DCHECK(JS_IsObject(proto), "a CSS rule was built in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_rule_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    r = calloc(1, sizeof(*r));
    CHECK(r != NULL, "the CSS rule record allocation failed");
    r->type = type;
    r->parent_style_sheet = JS_DupValue(ctx, parent_style_sheet);
    r->parent_rule = JS_DupValue(ctx, parent_rule);
    /* A calloc'd JSValue is not JS_NULL — its tag is whatever zero means — so every field is placed here. */
    r->selector_text = JS_NULL;
    r->block_text = JS_NULL;
    r->style = JS_UNDEFINED;
    /* Only a §6.4.5 grouping rule gets one, because only a grouping rule HAS child CSS rules — an empty Array
       on an `@import` would read as a list that happens to be empty, which is a different fact. */
    r->child_rules = rule_type_is_grouping(type) ? JS_NewArray(ctx) : JS_NULL;
    CHECK(!JS_IsException(r->child_rules), "a CSS rule's child list could not be allocated");
    r->rule_list = JS_UNDEFINED;
    r->media = JS_NULL;
    r->href = JS_NULL;
    r->layer_name = JS_NULL;
    r->supports_text = JS_NULL;
    r->namespace_uri = JS_NULL;
    r->prefix = JS_NULL;
    JS_SetOpaque(obj, r);
    return obj;
}

/* A §6.4.3 CSSStyleRule over the two texts a parse produced for it. */
static JSValue style_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                              const char *selector_text, const char *block_text)
{
    JSValue obj;
    CssRuleData *r;

    DCHECK(selector_text != NULL && block_text != NULL,
           "a CSSStyleRule was built without both of the texts it IS — a rule with no selector matches nothing "
           "and a rule with no body is the lossy shape css_rule.h exists to refuse");
    obj = rule_new(ctx, PROTO_STYLE, RULE_TYPE_STYLE, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) return obj;
    r = JS_GetOpaque(obj, g_rule_class);
    JS_FreeValue(ctx, r->selector_text);
    JS_FreeValue(ctx, r->block_text);
    r->selector_text = JS_NewString(ctx, selector_text);
    r->block_text = JS_NewString(ctx, block_text);
    return obj;
}

/* A §7.3 CSSMediaRule over the `@media` rule's own prelude. §4.4's "create a MediaList object with a string
   text" is what turns that prelude into the collection, so the rule never holds the page's spelling — it holds
   what the media-query parser accepted, which is what `media.mediaText` and `conditionText` then answer and is
   why `@media aLL {}` reads back as `all`. */
static JSValue media_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                              const char *prelude)
{
    JSValue obj;
    CssRuleData *r;

    DCHECK(prelude != NULL, "a CSSMediaRule was built with no prelude — `@media {}` has an EMPTY media query "
                            "list, which is the empty string and not the absence of one");
    obj = rule_new(ctx, PROTO_MEDIA, RULE_TYPE_MEDIA, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) return obj;
    r = JS_GetOpaque(obj, g_rule_class);
    JS_FreeValue(ctx, r->media);
    r->media = media_list_new(ctx, prelude);
    return obj;
}

/* A §6.4.4 CSSImportRule over the `@import` at-rule's own prelude. CSS Cascade §2's grammar decides every one
   of its four texts, and a prelude that does not MATCH that grammar is not an `@import` at all — CSS Syntax
   drops an at-rule whose grammar failed, which is JS_UNDEFINED here and the same answer lexbor gives for a
   `@media` it could not parse. The `media` is a §4.4 MediaList over the query-list tail for the reason §7.3's
   is: §6.4.4 says `media` returns the associated sheet's media list, and CSS Cascade §2 says that sheet's
   media list IS the one the at-rule declared. */
static JSValue import_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                               const char *prelude)
{
    CssImportPrelude p;
    JSValue obj;
    CssRuleData *r;

    DCHECK(prelude != NULL, "a CSSImportRule was built with no prelude");
    if (!css_prelude_import(prelude, strlen(prelude), &p)) return JS_UNDEFINED;
    obj = rule_new(ctx, PROTO_IMPORT, RULE_TYPE_IMPORT, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) { css_import_prelude_free(&p); return obj; }
    r = JS_GetOpaque(obj, g_rule_class);
    JS_FreeValue(ctx, r->href);
    r->href = JS_NewString(ctx, p.href);
    if (p.layer_name) { JS_FreeValue(ctx, r->layer_name); r->layer_name = JS_NewString(ctx, p.layer_name); }
    if (p.supports_text) {
        JS_FreeValue(ctx, r->supports_text);
        r->supports_text = JS_NewString(ctx, p.supports_text);
    }
    JS_FreeValue(ctx, r->media);
    r->media = media_list_new(ctx, p.media_text);
    css_import_prelude_free(&p);
    return obj;
}

/* A §6.4.9 CSSNamespaceRule. Same shape and the same reason for dropping: CSS Namespaces §2's production is
   `<namespace-prefix>? [ <string> | <url> ]` and nothing else is an `@namespace` rule. Lexbor accepts ANY
   prelude for this at-rule (it consumes tokens to the `;` and keeps only the offsets), so this grammar is the
   only thing between `@namespace a b c;` and a rule in `cssRules` that no browser has. */
static JSValue namespace_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                                  const char *prelude)
{
    char *prefix = NULL, *uri = NULL;
    JSValue obj;
    CssRuleData *r;

    DCHECK(prelude != NULL, "a CSSNamespaceRule was built with no prelude");
    if (!css_prelude_namespace(prelude, strlen(prelude), &prefix, &uri)) return JS_UNDEFINED;
    obj = rule_new(ctx, PROTO_NAMESPACE, RULE_TYPE_NAMESPACE, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) { free(prefix); free(uri); return obj; }
    r = JS_GetOpaque(obj, g_rule_class);
    JS_FreeValue(ctx, r->prefix);
    JS_FreeValue(ctx, r->namespace_uri);
    r->prefix = JS_NewString(ctx, prefix);
    r->namespace_uri = JS_NewString(ctx, uri);
    free(prefix);
    free(uri);
    return obj;
}

/* A CSS Fonts §12.1 CSSFontFaceRule over the DESCRIPTORS its block declares. They are kept in `block_text`,
   which is where a style rule's declarations are kept and is read through the same two entries — so
   `rule.style` is a §6.6 declaration block over the rule's own storage, and CSS Fonts' CSSFontFaceDescriptors
   is a different PROTOTYPE over that identical record rather than a second place the descriptors live. */
static JSValue font_face_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                                  const char *block_text)
{
    JSValue obj;
    CssRuleData *r;

    DCHECK(block_text != NULL,
           "a CSSFontFaceRule was built with no descriptor text — `@font-face {}` declares NOTHING, which is "
           "the empty string, and the absence of one is a parse that did not report the block at all");
    obj = rule_new(ctx, PROTO_FONT_FACE, RULE_TYPE_FONT_FACE, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) return obj;
    r = JS_GetOpaque(obj, g_rule_class);
    JS_FreeValue(ctx, r->block_text);
    r->block_text = JS_NewString(ctx, block_text);
    return obj;
}

static void rule_orphan(JSContext *ctx, JSValueConst rule)
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

/* ---- building the objects a PARSE produced ---------------------------------------------------------------- */

/* One parse's rules, as the objects they become. The handles `cssom_parse_rules` hands back are INDICES into
   `built` (offset by one, so NULL keeps meaning "top level"), which is why a realloc of that array is harmless
   where a pointer into it would not be. A DROPPED rule pushes JS_UNDEFINED, so its own children are dropped
   with it rather than being re-parented to the top of the sheet. */
typedef struct {
    JSContext   *ctx;
    JSValueConst sheet;        /* §6.4.2's parent CSS style sheet, which every rule in this parse names */
    JSValueConst top_parent;   /* the enclosing rule of the TOP-LEVEL rules — JS_NULL at a sheet's level */
    JSValueConst top_list;     /* where the top-level rules go */
    JSValue     *built;
    unsigned     n_built, cap_built;
    unsigned     n_top;        /* how many top-level rules became objects */
    char         unbuilt[64];  /* the FIRST at-rule name §6.4 has no interface for, or "" */
} RuleBuild;

static void *build_push(RuleBuild *b, JSValue rule)   /* CONSUMES rule */
{
    if (b->n_built == b->cap_built) {
        unsigned cap = b->cap_built ? b->cap_built * 2 : 8;
        JSValue *grown = realloc(b->built, (size_t)cap * sizeof(*grown));

        CHECK(grown != NULL, "cssom: OOM collecting the rules one parse produced");
        b->built = grown;
        b->cap_built = cap;
    }
    b->built[b->n_built++] = rule;
    return (void *)(uintptr_t)b->n_built;   /* one-based, so NULL means "no enclosing rule" */
}

static void build_free(RuleBuild *b)
{
    unsigned i;

    for (i = 0; i < b->n_built; i++) JS_FreeValue(b->ctx, b->built[i]);
    free(b->built);
    b->built = NULL;
    b->n_built = b->cap_built = 0;
}

/* THE ONE AT-RULE THAT IS DROPPED RATHER THAN CRASHED ON, and it is a POSITIVE statement about the spec rather
   than a gap: CSSOM keeps the historical constant `CHARSET_RULE = 2` and declares NO CSSCharsetRule interface
   at all, so there is no object an `@charset` could become and every user agent leaves it out of `cssRules`.
   Every OTHER at-rule in the platform HAS an interface, so meeting one is a capability to build. */
static bool at_rule_dropped(const char *name) { return strcmp(name, "charset") == 0; }

static JSValue rule_from_parse(RuleBuild *b, const CssomRule *pr, JSValueConst parent_rule)
{
    if (!pr->at_name)
        return style_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude, pr->block ? pr->block : "");
    if (strcmp(pr->at_name, "media") == 0) {
        DCHECK(pr->has_block,
               "an `@media` rule reached the builder with no block. CSS Syntax makes a block at-rule without "
               "one INVALID, and cssom_parse_rules drops an invalid at-rule before it is ever reported, so a "
               "block-less `@media` here means the parse kept a rule it should have discarded");
        return media_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude);
    }
    /* CSS Cascade §2 makes `@import` a STATEMENT at-rule terminated by a semicolon, so `@import url(x) {}` is
       an at-rule whose grammar failed and CSS Syntax DROPS it. It is dropped HERE and not asserted against,
       because the shape reaches this file from the PAGE: lexbor parses an at-rule it does not know as
       `_CUSTOM`, which accepts a block, so this is malformed author CSS and not an engine invariant. */
    if (strcmp(pr->at_name, "import") == 0)
        return pr->has_block ? JS_UNDEFINED : import_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude);
    if (strcmp(pr->at_name, "namespace") == 0) {
        DCHECK(!pr->has_block,
               "an `@namespace` rule reached the builder WITH a block. Lexbor's own namespace state marks the "
               "parse failed the moment it meets one and converts the rule to `_UNDEF`, which the walk drops, "
               "so a block here means that conversion did not happen");
        return namespace_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude);
    }
    /* And the mirror of it: CSS Fonts §12 makes `@font-face` a BLOCK at-rule, so `@font-face;` is invalid and
       dropped. Lexbor keeps this one rather than converting it (its `font_face_end` stores the returned block
       whether or not there is one, where `media_end` checks), so the drop is this file's. */
    if (strcmp(pr->at_name, "font-face") == 0) {
        DCHECK(!pr->has_block || pr->block != NULL,
               "an `@font-face` rule reached the builder with a block whose DESCRIPTORS were not reported. "
               "cssom_parse_rules serializes an `@font-face` body as declarations precisely because it is one, "
               "so a null block beside a live one means that arm did not run");
        return pr->has_block ? font_face_rule_new(b->ctx, b->sheet, parent_rule, pr->block ? pr->block : "")
                             : JS_UNDEFINED;
    }
    if (at_rule_dropped(pr->at_name)) return JS_UNDEFINED;
    if (!b->unbuilt[0]) snprintf(b->unbuilt, sizeof b->unbuilt, "%s", pr->at_name);
    return JS_UNDEFINED;
}

static void *rule_built(void *ud, void *parent, const CssomRule *pr)
{
    RuleBuild *b = ud;
    JSValueConst parent_rule;
    JSValue list, rule;

    if (!parent) {
        parent_rule = b->top_parent;
        list = JS_DupValue(b->ctx, b->top_list);
    } else {
        unsigned i = (unsigned)((uintptr_t)parent - 1);

        DCHECK(i < b->n_built, "cssom_parse_rules handed back a rule handle this parse never issued");
        parent_rule = b->built[i];
        /* The enclosing rule was dropped, so this one has no list to go in and no parent to name. */
        if (JS_IsUndefined(parent_rule)) return build_push(b, JS_UNDEFINED);
        list = rule_child_rules(b->ctx, parent_rule);
    }
    rule = rule_from_parse(b, pr, parent_rule);
    if (JS_IsException(rule) || JS_IsUndefined(rule)) {
        JS_FreeValue(b->ctx, list);
        JS_FreeValue(b->ctx, rule);
        return build_push(b, JS_UNDEFINED);
    }
    JS_SetPropertyUint32(b->ctx, list, rules_len(b->ctx, list), JS_DupValue(b->ctx, rule));
    JS_FreeValue(b->ctx, list);
    if (!parent) b->n_top++;
    return build_push(b, rule);
}

/* THE CRASH THAT NAMES WHAT TO BUILD. It is a function so that the at-rule's own name is in the message: the
   reader of a `@WHY` is standing at the rule the page shipped, and "an at-rule" tells them nothing about which
   interface to write. */
static void rule_unbuilt_fail(const char *name)
{
    char msg[600];

    snprintf(msg, sizeof msg,
             "CSSOM §6.4 has no interface built for the at-rule `@%s`, so a stylesheet containing one cannot be "
             "represented. §6.4.4's CSSImportRule, §6.4.5's CSSGroupingRule, §6.4.9's CSSNamespaceRule, CSS "
             "Conditional §7.2's CSSConditionRule, §7.3's CSSMediaRule and CSS Fonts §12.1's CSSFontFaceRule "
             "are built; what remains is §6.4.7's CSSPageRule with §6.4.8's CSSMarginRule (both of which need "
             "§6.4.7's CSSPageDescriptors, a declaration block over the same rule-backed text CSS Fonts' "
             "descriptors already use), CSS Animations' CSSKeyframesRule and CSSKeyframeRule, CSS Conditional "
             "§7.4's CSSSupportsRule (whose condition needs CSS.supports to evaluate), CSS Cascade's "
             "CSSLayerBlockRule and CSSLayerStatementRule, and CSS Contain's CSSContainerRule. Build the one "
             "this names and mint it in rule_from_parse — do NOT skip the rule, because every index after it "
             "would then name a different rule than the page's", name);
    DFAIL(msg);
}

/* Run one parse into `b`, and answer how many TOP-LEVEL rules it produced OBJECTS for. */
static unsigned build_run(RuleBuild *b, const char *text, size_t len)
{
    b->built = NULL;
    b->n_built = b->cap_built = b->n_top = 0;
    b->unbuilt[0] = '\0';
    cssom_parse_rules(text, len, rule_built, b);
    return b->n_top;
}

void css_rule_build_sheet(JSContext *ctx, JSValueConst list, JSValueConst parent_sheet,
                          const char *text, size_t len)
{
    RuleBuild b;

    b.ctx = ctx;
    b.sheet = parent_sheet;
    b.top_parent = JS_NULL;
    b.top_list = list;
    build_run(&b, text, len);
    if (b.unbuilt[0]) rule_unbuilt_fail(b.unbuilt);
    build_free(&b);
}

/* ---- §6.4's INSERT A CSS RULE and REMOVE A CSS RULE --------------------------------------------------------- */

/* THE TYPE OF THE RULE AT `list[i]`, which is the only thing steps 5 and 6 ask about it. */
static uint16_t rule_type_at(JSContext *ctx, JSValueConst list, uint32_t i)
{
    JSValue rule = JS_GetPropertyUint32(ctx, list, i);
    CssRuleData *r = rule_of(rule);
    uint16_t type;

    DCHECK(r != NULL, "a CSS rule list holds something that is not a CSS rule — §6.4's insert is the one thing "
                      "that ever puts one in, and it asserts the same premise from the other side");
    type = r ? r->type : (uint16_t)RULE_TYPE_STYLE;
    JS_FreeValue(ctx, rule);
    return type;
}

static int rule_rank(uint16_t type)
{
    if (type == RULE_TYPE_IMPORT) return RANK_IMPORT;
    if (type == RULE_TYPE_NAMESPACE) return RANK_NAMESPACE;
    return RANK_OTHER;
}

/* §6.4 STEP 5 — "if new rule cannot be inserted into list at the zero-indexed position index due to
   constraints specified by CSS, throw a HierarchyRequestError", whose own note is "for example, a CSS style
   sheet cannot contain an @import at-rule after a style rule".
     - `nested` set (§6.4.5's insertRule, into a grouping rule): an `@import` and an `@namespace` cannot go
       inside one AT ALL — CSS Cascade §2 and CSS Namespaces §2 both state their position relative to a STYLE
       SHEET, and neither is a rule a conditional group may contain.
     - `nested` unset: the sheet's rules are rank-ordered (see the RANK_ enum), so the constraint is that the
       insertion must not break that order. Asking it in BOTH directions is what the tests pin from both
       sides: a style rule inserted at index 0 of a sheet holding an `@import` is refused by the same line that
       refuses an `@import` inserted after one.
   THE ONE RANK THIS BUILD CANNOT SEE is CSS Cascade §6.4.4.2's `@layer` STATEMENT rule, which shares
   `@import`'s position ("ignoring @charset, @supports-condition, and @layer statement rules"). It has no
   interface here, so no such rule can be in a list to be mis-ranked; when CSSLayerStatementRule lands it takes
   RANK_IMPORT and nothing else changes. */
static bool insert_position_ok(JSContext *ctx, JSValueConst list, uint32_t index, uint16_t type, bool nested)
{
    uint32_t n, i;
    int rank;

    if (nested) return type != RULE_TYPE_IMPORT && type != RULE_TYPE_NAMESPACE;
    rank = rule_rank(type);
    n = rules_len(ctx, list);
    for (i = 0; i < n; i++) {
        int other = rule_rank(rule_type_at(ctx, list, i));

        if (i < index ? other > rank : other < rank) return false;
    }
    return true;
}

/* §6.4's "list contains anything other than @import at-rules, and @namespace at-rules" — the condition BOTH
   step 6 of insert and step 4 of remove are stated over, so it is one function reached from two places rather
   than two spellings of one sentence. */
static bool list_is_only_import_and_namespace(JSContext *ctx, JSValueConst list)
{
    uint32_t n = rules_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        uint16_t t = rule_type_at(ctx, list, i);

        if (t != RULE_TYPE_IMPORT && t != RULE_TYPE_NAMESPACE) return false;
    }
    return true;
}

JSValue css_rule_list_insert(JSContext *ctx, JSValueConst list, JSValueConst parent_sheet,
                             JSValueConst parent_rule, uint32_t index, const char *text, bool nested)
{
    RuleBuild b;
    JSValue scratch, built;
    unsigned n;

    DCHECK(JS_IsArray(list), "§6.4's insert a CSS rule was given something that is not a CSS rule list");
    DCHECK(text != NULL, "§6.4's insert a CSS rule was given no rule text");
    /* STEP 2, FIRST and before the parse, which is the order the algorithm states: a bad index throws even for
       text that would not have parsed. `index > length` and NOT `>=` — appending at the very end is LEGAL, and
       that asymmetry against remove's `>=` is the whole reason both are spelled out here. */
    if (index > rules_len(ctx, list))
        return JS_ThrowDOMException(ctx, "IndexSizeError", "the index is past the end of the rule list");
    /* STEP 3 — "parse a CSS rule". CSS Syntax's parse-a-RULE is ONE rule and a syntax error for anything else,
       which is what a parse reporting a count other than one MEANS. It is built into a SCRATCH list so that a
       rule steps 5 and 6 refuse was never in the page's list at all. */
    scratch = JS_NewArray(ctx);
    CHECK(!JS_IsException(scratch), "cssom: the insertRule scratch list could not be allocated");
    b.ctx = ctx;
    b.sheet = parent_sheet;
    b.top_parent = parent_rule;
    b.top_list = scratch;
    n = build_run(&b, text, strlen(text));
    build_free(&b);
    /* An at-rule with NO INTERFACE crashes here and does not become a refusal. Until §6.4.4 and §6.4.9 landed
       the two names steps 5 and 6 single out were themselves unbuilt, so this site had to answer for them from
       the at-rule's NAME; both are rules now, the constraint steps below run on the OBJECT the parse made, and
       what is left at this line is only ever a capability to build. */
    if (b.unbuilt[0]) {
        JS_FreeValue(ctx, scratch);
        rule_unbuilt_fail(b.unbuilt);
        return JS_ThrowDOMException(ctx, "SyntaxError", "the rule could not be parsed as a single CSS rule");
    }
    /* STEP 4 — "if new rule is a syntax error, throw a SyntaxError". Nothing parsed, more than one rule did, or
       the one that did was the type the spec itself drops (`@charset`), which is not a rule a page inserts. */
    if (n != 1) {
        JS_FreeValue(ctx, scratch);
        return JS_ThrowDOMException(ctx, "SyntaxError", "the rule could not be parsed as a single CSS rule");
    }
    built = JS_GetPropertyUint32(ctx, scratch, 0);
    JS_FreeValue(ctx, scratch);
    DCHECK(css_rule_is(built),
           "§6.4's insert a CSS rule reached its constraint steps with something that is not a CSS rule");
    {
        CssRuleData *nr = rule_of(built);

        DCHECK(nr != NULL, "§6.4's insert a CSS rule lost the rule it just parsed");
        if (!insert_position_ok(ctx, list, index, nr->type, nested)) {          /* STEP 5 */
            JS_FreeValue(ctx, built);
            return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                        "a rule of this type cannot be inserted at this position");
        }
        /* STEP 6 — "if new rule is an @namespace at-rule, and list contains anything other than @import
           at-rules, and @namespace at-rules, throw an InvalidStateError". It is NOT step 5 restated: the
           position may be perfectly legal (an `@namespace` at index 0 of a sheet whose only other rule is a
           style rule passes the rank test) and the insertion is still refused, which is exactly what
           css/cssom/at-namespace.html asserts. */
        if (nr->type == RULE_TYPE_NAMESPACE && !list_is_only_import_and_namespace(ctx, list)) {
            JS_FreeValue(ctx, built);
            return JS_ThrowDOMException(ctx, "InvalidStateError",
                                        "an @namespace rule cannot be inserted into a style sheet that holds "
                                        "anything other than @import and @namespace rules");
        }
    }
    rules_insert_at(ctx, list, index, built);            /* STEP 7 */
    return JS_NewUint32(ctx, index);                     /* STEP 8 */
}

JSValue css_rule_list_delete(JSContext *ctx, JSValueConst list, uint32_t index)
{
    JSValue old;

    DCHECK(JS_IsArray(list), "§6.4's remove a CSS rule was given something that is not a CSS rule list");
    /* STEP 2 — `index >= length`, the asymmetry against insert's `>`. */
    if (index >= rules_len(ctx, list))
        return JS_ThrowDOMException(ctx, "IndexSizeError", "the index is at or past the end of the rule list");
    old = JS_GetPropertyUint32(ctx, list, index);        /* STEP 3 */
    DCHECK(css_rule_is(old), "§6.4's remove a CSS rule found something that is not a CSS rule at its index");
    /* STEP 4 — "if old rule is an @namespace at-rule, and list contains anything other than @import at-rules,
       and @namespace at-rules, throw an InvalidStateError". The list is the one the rule is still IN, so the
       test includes the rule being removed, which is why a sheet of nothing but namespaces can lose one. */
    if (css_rule_is(old) && rule_of(old)->type == RULE_TYPE_NAMESPACE &&
        !list_is_only_import_and_namespace(ctx, list)) {
        JS_FreeValue(ctx, old);
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "an @namespace rule cannot be removed from a style sheet that holds "
                                    "anything other than @import and @namespace rules");
    }
    rules_remove_at(ctx, list, index);                   /* STEP 5 */
    rule_orphan(ctx, old);                           /* STEP 6 */
    JS_FreeValue(ctx, old);
    return JS_UNDEFINED;
}

/* ---- §6.4's SERIALIZE A CSS RULE ---------------------------------------------------------------------------- */

typedef struct { char *s; size_t len, cap; } RBuf;

static void rbuf_add_n(RBuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        char *grown;

        while (cap < b->len + n + 1) cap *= 2;
        grown = realloc(b->s, cap);
        CHECK(grown != NULL, "cssom: OOM serializing a CSS rule");
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void rbuf_add(RBuf *b, const char *s) { rbuf_add_n(b, s, strlen(s)); }

/* "Indenting each item with two spaces" — every LINE of it, because an item may itself be a group rule whose
   serialization is several lines, and a browser indents the whole block rather than only its first line. */
static void rbuf_add_indented(RBuf *b, const char *s)
{
    const char *p = s;

    rbuf_add(b, "  ");
    for (; *p; p++) {
        rbuf_add_n(b, p, 1);
        if (*p == '\n') rbuf_add(b, "  ");
    }
}

/* One of the record's texts, copied out. NULL when the field is JS_NULL — a rule type that has no such text —
   or when the string conversion itself threw, which is the caller's pending exception. */
static char *rule_text_copy(JSContext *ctx, JSValueConst v, size_t *plen)
{
    size_t len = 0;
    const char *c;
    char *out;

    *plen = 0;
    if (!JS_IsString(v)) return NULL;
    c = JS_ToCStringLen(ctx, &len, v);
    if (!c) return NULL;
    out = malloc(len + 1);
    CHECK(out != NULL, "cssom: OOM copying a rule's text");
    memcpy(out, c, len);
    out[len] = '\0';
    JS_FreeCString(ctx, c);
    *plen = len;
    return out;
}

static bool rule_serialize(JSContext *ctx, JSValueConst rule, RBuf *out);

/* Every child of `rule`, serialized, in order, with the spec's own "filtering out empty strings" already done.
   `*pn` is how many survived; the caller frees the array and its entries. */
static char **rule_children_serialized(JSContext *ctx, JSValueConst rule, unsigned *pn)
{
    JSValue kids = rule_child_rules(ctx, rule);
    uint32_t n = rules_len(ctx, kids), i;
    char **out = NULL;
    unsigned kept = 0;

    *pn = 0;
    if (n) {
        out = calloc(n, sizeof(*out));
        CHECK(out != NULL, "cssom: OOM serializing a rule's nested rules");
    }
    for (i = 0; i < n; i++) {
        JSValue kid = JS_GetPropertyUint32(ctx, kids, i);
        RBuf kb = { NULL, 0, 0 };
        bool ok;

        DCHECK(css_rule_is(kid), "a CSS rule's child list holds something that is not a CSS rule");
        ok = rule_serialize(ctx, kid, &kb);
        JS_FreeValue(ctx, kid);
        if (ok && kb.s && kb.s[0]) out[kept++] = kb.s;
        else free(kb.s);
    }
    JS_FreeValue(ctx, kids);
    *pn = kept;
    return out;
}

static void serialized_free(char **v, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* §6.4's CSSStyleRule arm, stated as five steps over three pieces — the selector list, the declaration block
   and the nested rules. Step 2 is §6.6's serialize-a-CSS-declaration-block, which is where the shorthand
   consolidation loop runs; step 3 is this rule's own `cssRules`, which CSS Nesting fills. */
static bool style_rule_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule, RBuf *out)
{
    size_t sl = 0, bl = 0;
    char *sel = rule_text_copy(ctx, r->selector_text, &sl);
    char *block, *decls;
    char **kids;
    unsigned nk, i;

    if (!sel) return false;
    block = rule_text_copy(ctx, r->block_text, &bl);
    decls = block ? cssom_serialize_declarations(block, bl) : NULL;
    free(block);
    kids = rule_children_serialized(ctx, rule, &nk);
    rbuf_add_n(out, sel, sl);                 /* STEP 1 */
    rbuf_add(out, " {");
    free(sel);
    if (!decls && nk == 0) {                  /* STEP 4 */
        rbuf_add(out, " }");
    } else if (nk == 0) {                     /* STEP 5 */
        rbuf_add(out, " ");
        rbuf_add(out, decls);
        rbuf_add(out, " }");
    } else {
        /* "Otherwise: if decls is not null, prepend it to rules; for each rule in rules, append a newline
           followed by two spaces, then the rule; then append a newline followed by `}`." */
        if (decls) { rbuf_add(out, "\n"); rbuf_add_indented(out, decls); }
        for (i = 0; i < nk; i++) { rbuf_add(out, "\n"); rbuf_add_indented(out, kids[i]); }
        rbuf_add(out, "\n}");
    }
    free(decls);
    serialized_free(kids, nk);
    return true;
}

/* §6.4's CSSMediaRule arm: "@media", a SPACE, the media query list, a SPACE and "{", a newline, then each
   nested rule (filtering out empty strings, indented by two spaces, joined with newline), a newline and "}".
   THE FINAL NEWLINE BELONGS TO THE ITEMS AND NOT TO THE CLOSING BRACE, which is what makes `@media print {}`
   serialize as "@media print {\n}" rather than "@media print {\n\n}" — the shape every engine produces and the
   one css/cssom/serialize-media-rule.html asserts byte for byte, `@media {}`'s two spaces included. */
static bool media_rule_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule, RBuf *out)
{
    char *text = media_list_text(ctx, r->media);
    char **kids;
    unsigned nk, i;

    if (!text) return false;
    kids = rule_children_serialized(ctx, rule, &nk);
    rbuf_add(out, "@media ");
    rbuf_add(out, text);
    free(text);
    rbuf_add(out, " {\n");
    for (i = 0; i < nk; i++) { rbuf_add_indented(out, kids[i]); rbuf_add(out, "\n"); }
    rbuf_add(out, "}");
    serialized_free(kids, nk);
    return true;
}

/* One of the record's texts as a C string, or NULL when the field is JS_NULL — which for §6.4.4's `layerName`
   and `supportsText` is the attribute's own null and therefore a piece the serialization omits. */
static char *rule_opt_text(JSContext *ctx, JSValueConst v)
{
    size_t len = 0;

    return rule_text_copy(ctx, v, &len);
}

/* §6.4's CSSImportRule arm — `@import`, a SPACE, serialize-a-URL of the location, then the media query list
   preceded by a SPACE when it is not empty, then `;`.
   THE LAYER AND THE SUPPORTS CONDITION ARE IN IT, AND §6.4's PROSE DOES NOT MENTION THEM, because that prose
   predates both: CSS Cascade §2 added `[ layer | layer(<layer-name>) ]?` and `supports(...)` to the at-rule's
   own grammar, and a serialization that dropped them would not re-parse as the rule it came from — which is
   the one property every serialize-a-CSS-rule arm has to have, and which the author cascade's round-trip
   assertion checks from the other side. They go where the GRAMMAR puts them, between the URL and the media
   query list, which is also what css/cssom/cssimportrule.html pins byte for byte. */
static bool import_rule_serialize(JSContext *ctx, CssRuleData *r, RBuf *out)
{
    size_t hl = 0;
    char *href = rule_text_copy(ctx, r->href, &hl);
    char *layer, *supports, *media, *url;

    DCHECK(href != NULL,
           "a §6.4.4 import rule has no href, and there is no partial answer: CSS Cascade §2's grammar has no "
           "arm without the `<url>`, so import_rule_new refuses a prelude that lacks one");
    if (!href) return false;
    url = css_serialize_url(href, hl);
    free(href);
    rbuf_add(out, "@import ");
    rbuf_add(out, url);
    free(url);
    layer = rule_opt_text(ctx, r->layer_name);
    if (layer) {
        /* The EMPTY layer name is the anonymous `layer` keyword — a real value, and the reason `layerName`
           distinguishes "" from null. */
        rbuf_add(out, *layer ? " layer(" : " layer");
        if (*layer) { rbuf_add(out, layer); rbuf_add(out, ")"); }
        free(layer);
    }
    supports = rule_opt_text(ctx, r->supports_text);
    if (supports) {
        rbuf_add(out, " supports(");
        rbuf_add(out, supports);
        rbuf_add(out, ")");
        free(supports);
    }
    media = media_list_text(ctx, r->media);
    if (!media) return false;
    if (*media) { rbuf_add(out, " "); rbuf_add(out, media); }
    free(media);
    rbuf_add(out, ";");
    return true;
}

/* §6.4's CSSNamespaceRule arm, stated in one sentence: `@namespace`, a SPACE, the serialization AS AN
   IDENTIFIER of the prefix if there is one, a SPACE if there is one, the serialization AS URL of the
   namespaceURI, and `;`. */
static bool namespace_rule_serialize(JSContext *ctx, CssRuleData *r, RBuf *out)
{
    size_t pl = 0, ul = 0;
    char *prefix = rule_text_copy(ctx, r->prefix, &pl);
    char *uri = rule_text_copy(ctx, r->namespace_uri, &ul);
    char *piece;

    DCHECK(prefix != NULL && uri != NULL,
           "a §6.4.9 namespace rule is missing its prefix or its namespace. Both are written by the one "
           "creator, which stores the EMPTY STRING for the default namespace rather than nothing at all");
    if (!prefix || !uri) { free(prefix); free(uri); return false; }
    rbuf_add(out, "@namespace ");
    if (pl) {
        piece = css_serialize_identifier(prefix, pl);
        rbuf_add(out, piece);
        rbuf_add(out, " ");
        free(piece);
    }
    piece = css_serialize_url(uri, ul);
    rbuf_add(out, piece);
    free(piece);
    free(prefix);
    free(uri);
    rbuf_add(out, ";");
    return true;
}

/* §6.4's CSSFontFaceRule arm. The spec's own steps name each descriptor in a fixed order and then admit "need
   to define how the CSSFontFaceRule descriptors' values are serialized"; every step has the SAME shape — a
   SPACE, `name:`, a SPACE, the value, `;` — which is exactly what §6.6's serialize-a-CSS-declaration-block
   produces for the block once the leading space and the closing " }" are added. So the descriptors go through
   the ONE declaration-block serializer rather than through a second hand-listed loop that could disagree with
   it about `rule.style.cssText`, and the order is the rule's own (which is what Blink and WebKit report, and
   what css/cssom/CSSFontFaceRule.html declines to pin because engines differ). */
static bool font_face_rule_serialize(JSContext *ctx, CssRuleData *r, RBuf *out)
{
    size_t bl = 0;
    char *block = rule_text_copy(ctx, r->block_text, &bl);
    char *decls;

    DCHECK(block != NULL,
           "a CSS Fonts §12.1 font-face rule has no descriptor text. `@font-face {}` declares nothing, which "
           "is the EMPTY STRING, so a null here means the string conversion itself failed");
    if (!block) return false;
    decls = cssom_serialize_declarations(block, bl);
    free(block);
    rbuf_add(out, "@font-face {");
    if (decls) { rbuf_add(out, " "); rbuf_add(out, decls); }
    rbuf_add(out, " }");
    free(decls);
    return true;
}

static bool rule_serialize(JSContext *ctx, JSValueConst rule, RBuf *out)
{
    CssRuleData *r = rule_of(rule);

    DCHECK(r != NULL, "§6.4's serialize a CSS rule was invoked on something that is not a CSS rule");
    if (!r) return false;
    switch (r->type) {
    case RULE_TYPE_MEDIA:     return media_rule_serialize(ctx, r, rule, out);
    case RULE_TYPE_IMPORT:    return import_rule_serialize(ctx, r, out);
    case RULE_TYPE_NAMESPACE: return namespace_rule_serialize(ctx, r, out);
    case RULE_TYPE_FONT_FACE: return font_face_rule_serialize(ctx, r, out);
    default:
        DCHECK(r->type == RULE_TYPE_STYLE, "§6.4's serialize a CSS rule met a rule type it has no arm for");
        return style_rule_serialize(ctx, r, rule, out);
    }
}

/* ---- the members ------------------------------------------------------------------------------------------ */

enum { CR_PARENT_RULE = 0, CR_PARENT_STYLE_SHEET, CR_TYPE, CR_CSS_TEXT, CR_SELECTOR_TEXT, CR_CONDITION_TEXT,
       CR_MEDIA, CR_MATCHES, CR_CSS_RULES, CR_HREF, CR_IMPORT_MEDIA, CR_LAYER_NAME, CR_SUPPORTS_TEXT,
       CR_NAMESPACE_URI, CR_PREFIX };

static JSValue js_rule_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssRuleData *r;

    switch (magic) {
    /* "The parentRule attribute must return the parent CSS rule" — null for a top-level rule, and a real rule
       for one a grouping rule encloses. */
    case CR_PARENT_RULE:
        r = rule_here(ctx, this_val);
        return r ? JS_DupValue(ctx, r->parent_rule) : JS_EXCEPTION;
    /* "The parentStyleSheet attribute must return the parent CSS style sheet." The spec's own note is the whole
       of when it is null: "the only circumstance where null is returned when a rule has been removed." */
    case CR_PARENT_STYLE_SHEET:
        r = rule_here(ctx, this_val);
        return r ? JS_DupValue(ctx, r->parent_style_sheet) : JS_EXCEPTION;
    /* §6.4.2's deprecated `type`: the rule's own TYPE state item, which §6.4 says is "initialized when a rule
       is created and cannot change". The enumeration is FROZEN by CSSOM ("no new values will be added"), so a
       rule interface that lands later brings its own constant with it rather than needing one invented here. */
    case CR_TYPE:
        r = rule_here(ctx, this_val);
        return r ? JS_NewUint32(ctx, r->type) : JS_EXCEPTION;
    case CR_CSS_TEXT: {
        RBuf b = { NULL, 0, 0 };
        JSValue out;

        if (!rule_here(ctx, this_val)) return JS_EXCEPTION;
        if (!rule_serialize(ctx, this_val, &b)) { free(b.s); return JS_EXCEPTION; }
        out = JS_NewStringLen(ctx, b.s ? b.s : "", b.len);
        free(b.s);
        return out;
    }
    /* §6.4.3: "on getting, must return the result of serializing the rule's associated selector list" — which
       is what the parse handed over and what the setter below replaces. */
    case CR_SELECTOR_TEXT:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_STYLE, "CSSStyleRule");
        return r ? JS_DupValue(ctx, r->selector_text) : JS_EXCEPTION;
    /* CSS Conditional §7.3's CSSMediaRule-specific definition of the attribute §7.2 declares: "the
       conditionText attribute, on getting, must return the value of media.mediaText on the rule". So it is not
       a second copy of the condition — it is ONE read of the MediaList, which is where the condition lives. */
    case CR_CONDITION_TEXT: {
        char *text;
        JSValue out;

        r = rule_here_typed(ctx, this_val, RULE_TYPE_MEDIA, "CSSConditionRule");
        if (!r) return JS_EXCEPTION;
        text = media_list_text(ctx, r->media);
        if (!text) return JS_EXCEPTION;
        out = JS_NewString(ctx, text);
        free(text);
        return out;
    }
    /* §7.3: "The media attribute must return a MediaList object for the list of media queries specified with
       the @media at-rule." [SameObject], which is why the object is the record's and not minted per read. */
    case CR_MEDIA:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_MEDIA, "CSSMediaRule");
        return r ? JS_DupValue(ctx, r->media) : JS_EXCEPTION;
    /* §7.3: "The matches attribute returns true if the rule is in a stylesheet attached to a document whose
       Window matches this rule's media media query, and returns false otherwise." A removed rule has a null
       parent CSS style sheet (§6.4's remove-a-CSS-rule), which is the whole of the first conjunct this engine
       can answer — a sheet it still holds is one HTML §4.2.6 attached. The second conjunct is the ENVIRONMENT
       question, and it is CONCOLIC: media_query.h keys it on the document, so a page that branches on this
       explores both viewports and the cascade below resolves under whichever arm the flow took. */
    case CR_MATCHES: {
        MediaQuerySet *set;
        JSValue out;

        r = rule_here_typed(ctx, this_val, RULE_TYPE_MEDIA, "CSSMediaRule");
        if (!r) return JS_EXCEPTION;
        if (JS_IsNull(r->parent_style_sheet)) return JS_NewBool(ctx, false);
        set = media_list_query_set(ctx, r->media);
        out = media_query_matches_value(ctx, set);
        media_query_free(set);
        return out;
    }
    /* §6.4.4: "The href attribute must return the URL specified by the @import at-rule" — the SPECIFIED one,
       which the spec's own note distinguishes from the resolved one ("to get the resolved URL use the href
       attribute of the associated CSS style sheet"). */
    case CR_HREF:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_IMPORT, "CSSImportRule");
        return r ? JS_DupValue(ctx, r->href) : JS_EXCEPTION;
    /* §6.4.4: "The media attribute must return the value of the media attribute of the associated CSS style
       sheet." CSS Cascade §2 makes that sheet's media list the one the at-rule declared — the imported sheet
       applies "exactly as if wrapped in @media with the given conditions" — so it is THIS rule's MediaList and
       not a read through a sheet, which is also what makes the attribute answerable while the import has not
       been fetched. [SameObject], so the object is the record's. */
    case CR_IMPORT_MEDIA:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_IMPORT, "CSSImportRule");
        return r ? JS_DupValue(ctx, r->media) : JS_EXCEPTION;
    /* §6.4.4: "The layerName attribute must return the layer name declared in the at-rule itself, or an empty
       string if the layer is anonymous, or NULL if the at-rule does not declare a layer" — three answers, and
       the record stores all three because "" and null are different facts about the same rule. */
    case CR_LAYER_NAME:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_IMPORT, "CSSImportRule");
        return r ? JS_DupValue(ctx, r->layer_name) : JS_EXCEPTION;
    /* §6.4.4: "The supportsText attribute must return the <supports-condition> declared in the at-rule itself,
       or null if the at-rule does not declare a supports condition." */
    case CR_SUPPORTS_TEXT:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_IMPORT, "CSSImportRule");
        return r ? JS_DupValue(ctx, r->supports_text) : JS_EXCEPTION;
    /* §6.4.9: "The namespaceURI attribute must return the namespace of the @namespace at-rule." */
    case CR_NAMESPACE_URI:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_NAMESPACE, "CSSNamespaceRule");
        return r ? JS_DupValue(ctx, r->namespace_uri) : JS_EXCEPTION;
    /* §6.4.9: "The prefix attribute must return the prefix of the @namespace at-rule or the EMPTY STRING if
       there is no prefix" — so the default namespace answers "" and never null. */
    case CR_PREFIX:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_NAMESPACE, "CSSNamespaceRule");
        return r ? JS_DupValue(ctx, r->prefix) : JS_EXCEPTION;
    /* §6.4.5: "The cssRules attribute must return a CSSRuleList object for the child CSS rules." [SameObject],
       so the collection is remembered on the record — and it shares the very Array the children live in, which
       is what its liveness IS. */
    default:
        DCHECK(magic == CR_CSS_RULES, "a CSS rule attribute ran with a magic §6.4 does not declare");
        r = rule_here_grouping(ctx, this_val);
        if (!r) return JS_EXCEPTION;
        if (!JS_IsObject(r->rule_list))
            r->rule_list = css_rule_list_new(ctx, JS_DupValue(ctx, r->child_rules));
        return JS_DupValue(ctx, r->rule_list);
    }
}

/* §6.4.2 states the other half of `cssText` in one sentence — "on setting the cssText attribute must do
   nothing" — so the attribute is READ-WRITE in the IDL and its setter is a real, specified no-effect rather
   than an unbuilt one. Installing it without a setter would be a different behaviour a page can see:
   `rule.cssText = 'x'` throws a TypeError in strict mode against a getter-only accessor, where the spec says
   the assignment is simply ignored. It still CONVERTS its argument, because Web IDL's setter runs the
   CSSOMString conversion before the attribute's own steps and a `{toString(){throw}}` must throw. */
static JSValue js_rule_set_css_text(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)ctx; (void)this_val; (void)val; (void)magic;
    return JS_UNDEFINED;
}

/* The one thing a selector probe keeps: the SERIALIZED selector list lexbor accepted, which is the canonical
   form §6.4.3's getter must answer afterwards. A parse that produced no top-level style rule leaves `*out`
   NULL, which is the setter's "the algorithm returned null, do nothing". */
static void *css_rule_selector_probe(void *ud, void *parent, const CssomRule *pr)
{
    char **out = ud;

    if (parent || pr->at_name || *out || !pr->prelude || !*pr->prelude) return NULL;
    *out = strdup(pr->prelude);
    CHECK(*out != NULL, "cssom: OOM keeping a parsed selector list");
    return NULL;
}

/* §6.4.3's setter: "Run the parse a group of selectors algorithm on the given value. If the algorithm returns a
   non-null value replace the associated selector list with the returned value. Otherwise, if the algorithm
   returns a null value, DO NOTHING." An invalid selector is silently ignored — not a throw, and not a stored
   invalid string, which is why the value goes back through the parser rather than into the slot. */
static JSValue js_rule_set_selector(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    CssRuleData *r = rule_here_typed(ctx, this_val, RULE_TYPE_STYLE, "CSSStyleRule");
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

/* ---- §6.4.5's insertRule and deleteRule ------------------------------------------------------------------- */

/* §6.4.5: "The insertRule(rule, index) method must return the result of invoking insert a CSS rule rule into
   the child CSS rules at index, WITH THE NESTED FLAG SET." That flag is the whole difference from §6.1.2's, so
   it is passed to the one algorithm rather than re-derived inside it. */
static JSValue js_rule_insert_rule(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    CssRuleData *r = rule_here_grouping(ctx, this_val);
    const char *text;
    uint32_t index = 0;
    JSValue list, out;

    (void)magic;
    if (!r) return JS_EXCEPTION;
    DCHECK(argc >= 1, "§6.4.5's insertRule reached its body with no rule — its first IDL argument is required");
    /* Both arguments arrive CONVERTED: `CSSOMString rule` and `optional unsigned long index = 0` are the
       declaration's work, so nothing here runs the page's code and the default is the IDL's. */
    if (argc >= 2) JS_ToUint32(ctx, &index, argv[1]);
    text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    list = rule_child_rules(ctx, this_val);
    out = css_rule_list_insert(ctx, list, r->parent_style_sheet, this_val, index, text, /*nested*/ true);
    JS_FreeValue(ctx, list);
    JS_FreeCString(ctx, text);
    return out;
}

/* §6.4.5: "The deleteRule(index) method must remove a CSS rule from the child CSS rules at index." */
static JSValue js_rule_delete_rule(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    uint32_t index = 0;
    JSValue list, out;

    (void)magic;
    if (!rule_here_grouping(ctx, this_val)) return JS_EXCEPTION;
    DCHECK(argc >= 1, "§6.4.5's deleteRule reached its body with no index — its IDL argument is required");
    JS_ToUint32(ctx, &index, argv[0]);
    list = rule_child_rules(ctx, this_val);
    out = css_rule_list_delete(ctx, list, index);
    JS_FreeValue(ctx, list);
    return out;
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
    CssRuleData *r = rule_here_typed(ctx, this_val, RULE_TYPE_STYLE, "CSSStyleRule");

    (void)magic;
    if (!r) return JS_EXCEPTION;
    if (!JS_IsObject(r->style)) {
        JS_FreeValue(ctx, r->style);
        r->style = cssom_style_properties_for_rule(ctx, this_val);
    }
    return JS_DupValue(ctx, r->style);
}

/* CSS Fonts §12.1's `[SameObject, PutForwards=cssText] readonly attribute CSSFontFaceDescriptors style`. It is
   the SAME memo, over the same field, as §6.4.3's above — a rule has ONE declaration block object — and it is
   a separate member only because the two attributes are declared on two interfaces and hand back two different
   ones. Which interface the block IS is decided by the creator this calls, not by anything on the record. */
static JSValue js_rule_font_face_style(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssRuleData *r = rule_here_typed(ctx, this_val, RULE_TYPE_FONT_FACE, "CSSFontFaceRule");

    (void)magic;
    if (!r) return JS_EXCEPTION;
    if (!JS_IsObject(r->style)) {
        JS_FreeValue(ctx, r->style);
        r->style = cssom_font_face_descriptors_for_rule(ctx, this_val);
    }
    return JS_DupValue(ctx, r->style);
}

/* THE RULE'S DECLARATIONS, as the text they are. §6.6's block reads them through here and writes them back
   through the setter below, so the two components share ONE storage rather than each keeping a copy that could
   disagree — which is the same reason an element's block is the `style` attribute and not a parsed cache.
   OWNED: the caller frees. NULL for a rule whose body is empty and for one that has no body at all. */
char *css_rule_block_text(JSContext *ctx, JSValueConst rule, size_t *plen)
{
    CssRuleData *r = rule_of(rule);
    char *out;

    DCHECK(r != NULL, "a rule's declaration block was read off something that is not a CSS rule");
    DCHECK(plen != NULL, "a rule's declaration block was read with nowhere to report its length");
    out = rule_text_copy(ctx, r->block_text, plen);
    if (out && *plen == 0) { free(out); return NULL; }
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
    DCHECK(r->type == RULE_TYPE_STYLE || r->type == RULE_TYPE_FONT_FACE,
           "§6.6's declaration block wrote its text back onto a rule that HAS no declaration block. A rule's "
           "`style` attribute is the only thing that reaches this, and only §6.4.3's CSSStyleRule and CSS "
           "Fonts §12.1's CSSFontFaceRule declare one in this build — the day §6.4.7's CSSPageRule lands, its "
           "CSSPageDescriptors comes with it");
    JS_FreeValue(ctx, r->block_text);
    r->block_text = JS_NewStringLen(ctx, text, len);
}

/* ---- the AUTHOR CASCADE's view ----------------------------------------------------------------------------- */

static bool cascade_emit(JSContext *ctx, JSValueConst list, RBuf *out, uint32_t *pn);

/* One rule's contribution to the text the selector matcher re-parses. A STYLE rule contributes itself; a
   CONDITIONAL GROUP rule contributes its children when its condition holds and nothing when it does not, which
   is what `@media` MEANS and is the whole reason the cascade cannot simply read a sheet's top level. */
static bool cascade_emit_one(JSContext *ctx, JSValueConst rule, RBuf *out, uint32_t *pn)
{
    CssRuleData *r = rule_of(rule);
    size_t sl = 0, bl = 0;
    char *sel, *block;

    DCHECK(r != NULL,
           "a CSS style sheet's rule list holds something that is not a CSS rule — §6.4's insert is the only "
           "thing that ever puts one in, and it asserts the same premise from the other side");
    if (!r) return false;
    if (r->type == RULE_TYPE_MEDIA) {
        MediaQuerySet *set = media_list_query_set(ctx, r->media);
        bool applies = media_query_matches_now(ctx, set);
        JSValue kids;
        bool ok;

        media_query_free(set);
        if (!applies) return true;
        kids = rule_child_rules(ctx, rule);
        ok = cascade_emit(ctx, kids, out, pn);
        JS_FreeValue(ctx, kids);
        return ok;
    }
    /* AN `@namespace` IS EMITTED, and it is the one non-style rule that must be: it declares a prefix the
       SELECTORS below are written against, so a sheet whose `@namespace svg url(...)` were dropped would hand
       lexbor `svg|a { … }` with no `svg` bound — an invalid selector, which the parse then drops, which
       silently un-styles every element the page selected that way. It goes in verbatim (the serialization is
       the canonical form, terminated by its own `;`) and it COUNTS, because `*pn` is what the caller's
       round-trip assertion compares the re-parse's rule count against. */
    if (r->type == RULE_TYPE_NAMESPACE) {
        RBuf one = { NULL, 0, 0 };

        if (!namespace_rule_serialize(ctx, r, &one)) { free(one.s); return false; }
        rbuf_add(out, one.s);
        free(one.s);
        (*pn)++;
        return true;
    }
    /* AN `@import` AND AN `@font-face` CONTRIBUTE NO STYLE RULE TO THIS SHEET, and each for its own reason
       rather than for a shared "not handled". An `@import`'s declarations belong to the IMPORTED sheet — CSS
       Cascade §2 treats its contents "as if they were written in place of the @import rule" — and this build
       fetches no imported sheet, which css_rule.h records as the gap that `styleSheet` is absent for. An
       `@font-face` declares a FONT FACE and not a style: nothing it contains can match an element, so it is
       not a rule the selector matcher has anything to do with. */
    if (r->type == RULE_TYPE_IMPORT || r->type == RULE_TYPE_FONT_FACE) return true;
    DCHECK(r->type == RULE_TYPE_STYLE, "the author cascade met a rule type it has no arm for");
    /* CSS NESTING IS NOT FLATTENED, and must not be: a nested style rule's selector is RELATIVE to its parent's
       (`&`, and the implicit descendant a bare compound selector carries), so lifting it to the top level of
       the re-parsed text would make it match elements the page never selected. The resolution is CSS Nesting's
       own — build it, over the parent's selector list, rather than letting the rule style the wrong subtree. */
    DCHECK(rules_len(ctx, r->child_rules) == 0,
           "a §6.4.3 style rule has NESTED rules and the author cascade has no CSS Nesting: a nested rule's "
           "selector is relative to its parent's, so it cannot be flattened into the sheet's top level. Build "
           "css-nesting-1 §2's nest-containing resolution (the nested selector becomes `:is(parent) sel`) and "
           "emit the RESOLVED selector here");
    sel = rule_text_copy(ctx, r->selector_text, &sl);
    /* A rule with NO selector text cannot be serialized into a sheet at all, and there is no partial answer
       worth giving: an empty prelude would make lexbor drop the rule and every index after it would name a
       neighbour's declarations, and a `*` stand-in would make the rule match EVERY element. §6.4.3's selector
       list is non-empty for every rule this build can make — both creators keep only what lexbor accepted — so
       this is the pending-exception path, and the whole sheet is abandoned. */
    DCHECK(sel != NULL,
           "a §6.4.3 style rule has no serialized selector list. Both things that write one — the parse and "
           "`selectorText =`, in css_rule.c — go through cssom_parse_rules and store only what lexbor "
           "accepted, so an empty one means the string conversion itself failed");
    if (!sel) return false;
    block = rule_text_copy(ctx, r->block_text, &bl);
    rbuf_add_n(out, sel, sl);
    rbuf_add(out, "{");
    if (block) rbuf_add_n(out, block, bl);
    rbuf_add(out, "}");
    free(sel);
    free(block);
    (*pn)++;
    return true;
}

static bool cascade_emit(JSContext *ctx, JSValueConst list, RBuf *out, uint32_t *pn)
{
    uint32_t n = rules_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue rule = JS_GetPropertyUint32(ctx, list, i);
        bool ok = cascade_emit_one(ctx, rule, out, pn);

        JS_FreeValue(ctx, rule);
        if (!ok) return false;
    }
    return true;
}

char *css_rule_cascade_text(JSContext *ctx, JSValueConst list, uint32_t *pn)
{
    RBuf out = { NULL, 0, 0 };

    DCHECK(pn != NULL, "the author cascade's text was built with nowhere to report how many rules went in");
    *pn = 0;
    if (!cascade_emit(ctx, list, &out, pn)) {
        free(out.s);
        *pn = 0;
        return NULL;
    }
    return out.s;
}

/* ---- the interfaces -------------------------------------------------------------------------------------- */

/* §6.4.2's historical constants, which the IDL declares on the interface AND its prototype. */
static const struct { const char *name; uint32_t v; } CR_CONSTS[] = {
    { "STYLE_RULE", 1 }, { "CHARSET_RULE", 2 }, { "IMPORT_RULE", 3 }, { "MEDIA_RULE", 4 },
    { "FONT_FACE_RULE", 5 }, { "PAGE_RULE", 6 }, { "MARGIN_RULE", 9 }, { "NAMESPACE_RULE", 10 },
    /* CSS Conditional §7.1's `partial interface CSSRule` — the one addition to a list CSSOM calls frozen, and
       it is here because that standard puts it there rather than because a number was needed. */
    { "SUPPORTS_RULE", 12 },
};

static void rule_install_constants(JSContext *ctx, JSValueConst target)
{
    unsigned i;

    for (i = 0; i < sizeof(CR_CONSTS) / sizeof(CR_CONSTS[0]); i++)
        JS_DefinePropertyValueStr(ctx, target, CR_CONSTS[i].name,
                                  JS_NewUint32(ctx, CR_CONSTS[i].v), JS_PROP_ENUMERABLE);
}

void css_rule_init(JSContext *ctx)
{
    JSClassDef d = { "CSSRule", rule_finalizer, rule_gc_mark };

    if (g_rule_class) return;   /* one AGENT, one class and one set of pool entries */
    JS_NewClassID(JS_GetRuntime(ctx), &g_rule_class);
    JS_NewClass(JS_GetRuntime(ctx), g_rule_class, &d);
    g_proto_slot[PROTO_RULE] = realm_value_declare(ctx, "CSSOM §6.4.2 CSSRule.prototype");
    g_proto_slot[PROTO_GROUPING] = realm_value_declare(ctx, "CSSOM §6.4.5 CSSGroupingRule.prototype");
    g_proto_slot[PROTO_STYLE] = realm_value_declare(ctx, "CSSOM §6.4.3 CSSStyleRule.prototype");
    g_proto_slot[PROTO_CONDITION] = realm_value_declare(ctx, "CSS Conditional §7.2 CSSConditionRule.prototype");
    g_proto_slot[PROTO_MEDIA] = realm_value_declare(ctx, "CSS Conditional §7.3 CSSMediaRule.prototype");
    g_proto_slot[PROTO_IMPORT] = realm_value_declare(ctx, "CSSOM §6.4.4 CSSImportRule.prototype");
    g_proto_slot[PROTO_NAMESPACE] = realm_value_declare(ctx, "CSSOM §6.4.9 CSSNamespaceRule.prototype");
    g_proto_slot[PROTO_FONT_FACE] = realm_value_declare(ctx, "CSS Fonts §12.1 CSSFontFaceRule.prototype");
    g_id_set_selector = idl_setter_id(ctx, IDL_DOMSTRING, false, js_rule_set_selector, 0);
    g_id_set_css_text = idl_setter_id(ctx, IDL_DOMSTRING, false, js_rule_set_css_text, 0);
    {
        /* §6.4.5: `unsigned long insertRule(CSSOMString rule, optional unsigned long index = 0)` and
           `undefined deleteRule(unsigned long index)` — the same two shapes §6.1.2 declares, because they are
           the same two algorithms. */
        static const IdlArgType INSERT[2] = { IDL_DOMSTRING, IDL_UNSIGNED_LONG };
        static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };

        g_id_insert_rule = idl_method_id(ctx, INSERT, 2, js_rule_insert_rule, 0);
        idl_optional_from(1);
        g_id_delete_rule = idl_method_id(ctx, ONE_ULONG, 1, js_rule_delete_rule, 0);
    }
    realm_declare_intrinsic(css_rule_install_proto);
}

void css_rule_install_proto(JSContext *ctx)
{
    JSValue base, grouping, style, condition, media, import_rule, ns, font_face;

    DCHECK(g_rule_class != 0, "a realm asked for the rule prototypes before the interfaces existed");

    /* §6.4.2's CSSRule.prototype. Nothing is an instance of it — §6.4.2 is "an abstract, base CSS rule" — and
       nothing is an instance of the two abstract prototypes chained above it either; each is nonetheless the
       REALM's own, because a C member runs in the realm that DEFINED it. */
    base = JS_NewObject(ctx);
    CHECK(!JS_IsException(base), "CSSRule.prototype could not be allocated");
    idl_interface_tag(ctx, base, "CSSRule");
    idl_install_accessor(ctx, base, "cssText", js_rule_get, CR_CSS_TEXT, g_id_set_css_text);
    idl_install_accessor(ctx, base, "parentRule", js_rule_get, CR_PARENT_RULE, -1);
    idl_install_accessor(ctx, base, "parentStyleSheet", js_rule_get, CR_PARENT_STYLE_SHEET, -1);
    idl_install_accessor(ctx, base, "type", js_rule_get, CR_TYPE, -1);
    rule_install_constants(ctx, base);

    /* §6.4.5's CSSGroupingRule.prototype — "an at-rule that contains other rules nested inside itself", and
       also what a STYLE rule is since CSS Nesting, which is why the IDL says `CSSStyleRule : CSSGroupingRule`
       and why both concrete prototypes below chain through this one. */
    grouping = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(grouping), "CSSGroupingRule.prototype could not be allocated");
    idl_interface_tag(ctx, grouping, "CSSGroupingRule");
    idl_install_accessor(ctx, grouping, "cssRules", js_rule_get, CR_CSS_RULES, -1);
    idl_install_method(ctx, grouping, "insertRule", 1, g_id_insert_rule);
    idl_install_method(ctx, grouping, "deleteRule", 1, g_id_delete_rule);

    style = JS_NewObjectProto(ctx, grouping);
    CHECK(!JS_IsException(style), "CSSStyleRule.prototype could not be allocated");
    idl_interface_tag(ctx, style, "CSSStyleRule");
    idl_install_accessor(ctx, style, "selectorText", js_rule_get, CR_SELECTOR_TEXT, g_id_set_selector);
    idl_install_accessor(ctx, style, "style", js_rule_style, 0, cssom_put_forwards_setter());

    /* CSS Conditional §7.2's CSSConditionRule.prototype — "all the conditional at-rules, which consist of a
       condition and a statement block". `conditionText` is READONLY: the setter older drafts gave it is gone
       from the IDL, and installing one would be a member the platform does not have. */
    condition = JS_NewObjectProto(ctx, grouping);
    CHECK(!JS_IsException(condition), "CSSConditionRule.prototype could not be allocated");
    idl_interface_tag(ctx, condition, "CSSConditionRule");
    idl_install_accessor(ctx, condition, "conditionText", js_rule_get, CR_CONDITION_TEXT, -1);

    media = JS_NewObjectProto(ctx, condition);
    CHECK(!JS_IsException(media), "CSSMediaRule.prototype could not be allocated");
    idl_interface_tag(ctx, media, "CSSMediaRule");
    idl_install_accessor(ctx, media, "media", js_rule_get, CR_MEDIA, media_list_put_forwards_setter());
    idl_install_accessor(ctx, media, "matches", js_rule_get, CR_MATCHES, -1);

    /* §6.4.4's CSSImportRule.prototype. It derives from CSSRule and NOT from CSSGroupingRule — an `@import`
       contains no rules — which is why `cssRules`, `insertRule` and `deleteRule` are unreachable on one and
       why the three grouping members brand-check their receiver.
       `styleSheet` IS ABSENT, AND THAT IS THE HONEST ANSWER RATHER THAN A NULL. §6.4.4 declares
       `[SameObject] readonly attribute CSSStyleSheet? styleSheet` and defines it as "the associated CSS style
       sheet, if any, or null otherwise", with a note giving the one case that produces null: an import whose
       supports() condition does not match. This engine FETCHES NO IMPORTED SHEET — there is no CSS resource
       load at all, not for `@import` and not for `<link rel=stylesheet>` — so a getter here could only ever
       answer null, and that null is byte-for-byte the spec's own real answer for a different rule. A page
       (and this engine's own reader) could not tell "there is no sheet" from "this build never fetched it",
       which is the one shape §NO STUBS forbids: a wrong answer that reads exactly like a right one. Absent, it
       is a TypeError the page names and a MISSING member the IDL gap audit reports. The capability to build is
       the sheet fetch, and it is named in css_rule.h. */
    import_rule = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(import_rule), "CSSImportRule.prototype could not be allocated");
    idl_interface_tag(ctx, import_rule, "CSSImportRule");
    idl_install_accessor(ctx, import_rule, "href", js_rule_get, CR_HREF, -1);
    idl_install_accessor(ctx, import_rule, "media", js_rule_get, CR_IMPORT_MEDIA,
                         media_list_put_forwards_setter());
    idl_install_accessor(ctx, import_rule, "layerName", js_rule_get, CR_LAYER_NAME, -1);
    idl_install_accessor(ctx, import_rule, "supportsText", js_rule_get, CR_SUPPORTS_TEXT, -1);

    /* §6.4.9's CSSNamespaceRule.prototype — two readonly strings and nothing else. */
    ns = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(ns), "CSSNamespaceRule.prototype could not be allocated");
    idl_interface_tag(ctx, ns, "CSSNamespaceRule");
    idl_install_accessor(ctx, ns, "namespaceURI", js_rule_get, CR_NAMESPACE_URI, -1);
    idl_install_accessor(ctx, ns, "prefix", js_rule_get, CR_PREFIX, -1);

    /* CSS Fonts §12.1's CSSFontFaceRule.prototype. Its `style` is a CSSFontFaceDescriptors and not a
       CSSStyleProperties, which is a real difference a page reads (`[object CSSFontFaceDescriptors]`, and a
       `unicode-range` attribute the other interface does not have) — see core/css/css_style_declaration.h.
       The [PutForwards=cssText] setter is the shared one, because §3.4.4's forwarding is a [[Get]] of `style`
       by NAME followed by a [[Set]] of `cssText`, which does not care which interface answered. */
    font_face = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(font_face), "CSSFontFaceRule.prototype could not be allocated");
    idl_interface_tag(ctx, font_face, "CSSFontFaceRule");
    idl_install_accessor(ctx, font_face, "style", js_rule_font_face_style, 0, cssom_put_forwards_setter());

    /* Each into the realm's own slot, which asserts on its own that this install ran once in this realm. */
    realm_value_set(ctx, g_proto_slot[PROTO_IMPORT], import_rule);
    realm_value_set(ctx, g_proto_slot[PROTO_NAMESPACE], ns);
    realm_value_set(ctx, g_proto_slot[PROTO_FONT_FACE], font_face);
    realm_value_set(ctx, g_proto_slot[PROTO_RULE], base);
    realm_value_set(ctx, g_proto_slot[PROTO_GROUPING], grouping);
    realm_value_set(ctx, g_proto_slot[PROTO_STYLE], style);
    realm_value_set(ctx, g_proto_slot[PROTO_CONDITION], condition);
    realm_value_set(ctx, g_proto_slot[PROTO_MEDIA], media);
}

void css_rule_install(JSContext *ctx, JSValueConst global)
{
    /* IN INHERITANCE ORDER, because each interface object's [[Prototype]] is the one before it: Web IDL §3.7.1
       says "the interface object for a non-callback interface that inherits from another interface must have
       its [[Prototype]] set to the interface object of the inherited interface", and that is not decoration —
       it is how `CSSMediaRule.STYLE_RULE` reads §6.4.2's constant and how `CSSStyleRule.__proto__ ===
       CSSGroupingRule` answers true. `inherits` is the index of the interface this one derives from, or -1 for
       the root, so the chain is stated once as data rather than as five assignments. */
    static const struct { const char *name; int slot; int inherits; } IFACES[] = {
        { "CSSRule",           PROTO_RULE,      -1 },
        { "CSSGroupingRule",   PROTO_GROUPING,   0 },
        { "CSSStyleRule",      PROTO_STYLE,      1 },
        { "CSSConditionRule",  PROTO_CONDITION,  1 },
        { "CSSMediaRule",      PROTO_MEDIA,      3 },
        /* Each of the three derives from CSSRule directly — none of them contains rules. */
        { "CSSImportRule",     PROTO_IMPORT,     0 },
        { "CSSNamespaceRule",  PROTO_NAMESPACE,  0 },
        { "CSSFontFaceRule",   PROTO_FONT_FACE,  0 },
    };
    JSValue iface[sizeof(IFACES) / sizeof(IFACES[0])];
    unsigned i, n = sizeof(IFACES) / sizeof(IFACES[0]);

    DCHECK(g_rule_class != 0, "the rule interfaces were installed before css_rule_init declared them");
    for (i = 0; i < n; i++) {
        JSValue proto = realm_value_get(ctx, g_proto_slot[IFACES[i].slot]);

        DCHECK(JS_IsObject(proto),
               "the rule interfaces were installed in a realm that never ran their prototype install");
        iface[i] = idl_interface_object(ctx, IFACES[i].name, proto);
        JS_FreeValue(ctx, proto);
        DCHECK(IFACES[i].inherits < (int)i,
               "an interface object was chained to one that has not been built yet — the table above is in "
               "inheritance order so that a base always precedes what derives from it");
        if (IFACES[i].inherits >= 0)
            JS_SetPrototype(ctx, iface[i], iface[IFACES[i].inherits]);
    }
    /* §6.4.2's constants are on the INTERFACE OBJECT as well as the prototype, which is what Web IDL says of a
       `const` and what `CSSRule.STYLE_RULE` reads. Only the base declares them, because the chain above is
       what carries them to every interface that inherits. */
    rule_install_constants(ctx, iface[0]);
    for (i = 0; i < n; i++)
        JS_SetPropertyStr(ctx, global, IFACES[i].name, iface[i]);
}

void css_rule_free(JSRuntime *rt)
{
    (void)rt;   /* every prototype is the REALM's — released with its context */
}
