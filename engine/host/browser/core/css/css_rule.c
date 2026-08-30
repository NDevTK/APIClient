/* CSSOM §6.4's CSS rules, CSS Conditional §7.2/§7.3's conditional group rule and CSS Paged Media's page and
 * margin rules. See css_rule.h for why a rule is made of text, why a grouping rule's child list is a JS Array,
 * and why one class carries every interface.
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
#include "core/css/css_nesting.h"
#include "core/css/css_page.h"
#include "core/css/css_property_syntax.h"
#include "core/css/css_syntax_match.h"
#include "core/css/css_rule.h"
#include "core/css/css_rule_list.h"
#include "core/css/css_serialize.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_supports.h"
#include "core/css/media_list.h"
#include "core/css/media_query.h"
#include "core/idl_args.h"
#include "core/idl_indexed.h"
#include "core/realm.h"
#include "solver/cow.h"

/* §6.4's TYPE state item, which IS which interface this rule is. For every interface §6.4.2's `type` table
   NAMES, the discriminator and that table's number are ONE number rather than a stored type beside an interface
   tag that could disagree with it — CSS Animations §6.1.1's `partial interface CSSRule` adds the two in the
   middle, exactly as CSS Conditional §7.1 adds SUPPORTS_RULE to a list CSSOM calls frozen.
   AND THE TABLE RAN OUT, WHICH IS THE SPEC'S OWN DECISION RATHER THAN A GAP TO INVENT A NUMBER FOR. §6.4.2's
   `type` ends "Otherwise: return 0" and attaches the reason: "this enumeration is thus FROZEN in its current
   state, and no new values will be added to reflect additional at-rules; all at-rules beyond the ones listed
   above will return 0." So CSS Cascade §8.1's and §8.2's `@layer` interfaces have no number at all, and neither
   will the next interface that lands. The discriminator therefore CONTINUES PAST the table and
   `rule_legacy_type` maps it back — one fact split into two the moment they stopped agreeing, exactly as
   `rule_type_has_child_rules` and `rule_type_is_grouping` are. */
enum { RULE_TYPE_STYLE = 1, RULE_TYPE_IMPORT = 3, RULE_TYPE_MEDIA = 4, RULE_TYPE_FONT_FACE = 5,
       RULE_TYPE_PAGE = 6, RULE_TYPE_KEYFRAMES = 7, RULE_TYPE_KEYFRAME = 8, RULE_TYPE_MARGIN = 9,
       RULE_TYPE_NAMESPACE = 10,
       /* CSS Conditional §7.1 "Extensions to the CSSRule interface" — `const unsigned short SUPPORTS_RULE =
          12`, the number that standard adds to the list CSSOM calls frozen. 11 (CSS Counter Styles 3 §9.1's)
          and 14 (CSS Fonts 4 §12.2's) are DECLARED as constants below and have no interface behind them, which
          is why they are not here: this enum is the interfaces, and CR_CONSTS is the historical table. */
       RULE_TYPE_SUPPORTS = 12,
       /* At and above this, §6.4.2's `type` answers 0 — the interfaces its frozen table does not name. */
       RULE_TYPE_UNNUMBERED = 0x100,
       RULE_TYPE_LAYER_BLOCK = RULE_TYPE_UNNUMBERED, RULE_TYPE_LAYER_STATEMENT,
       /* CSS Properties and Values API 1 §6.1's CSSPropertyRule — also numberless, and for the same reason the
          two above it are: §6.4.2's table is frozen and that standard adds no `partial interface CSSRule` to
          it, so `propertyRule.type` is 0. */
       RULE_TYPE_PROPERTY,
       /* CSS Conditional 5 §9.1's CSSContainerRule — numberless too, and it is the clearest case of the
          sentence above being the SPEC'S decision rather than a gap: CSS Conditional adds a `partial interface
          CSSRule` for §7.1's SUPPORTS_RULE = 12 and adds NONE for the interface it declares in §9.1, which is
          §6.4.2's freeze taking effect inside one standard. */
       RULE_TYPE_CONTAINER };

/* WHERE A RULE MAY SIT IN A STYLE SHEET. A sheet's rules are a PROLOGUE followed by a body, and three standards
   write that prologue between them:
     CSS Cascade §2 — "any @import rules must precede all other valid at-rules and style rules in a style sheet
       (ignoring @charset, @supports-condition, and @layer statement rules) and must not have any other valid
       at-rules or style rules between it and previous @import rules, or else the @import rule is invalid";
     CSS Namespaces §2 — "any @namespace rules must follow all @charset and @import rules and precede all other
       non-ignored at-rules and style rules";
     CSS Cascade §6.4.4.2 — "such empty @layer rules are allowed BEFORE @import and @namespace rules (after the
       @charset rule, if any) AS WELL AS everywhere @layer block at-rules are allowed", whose note spells out
       what that costs: "no @layer rules are allowed between @import and @namespace rules. Any @layer rule that
       comes after an @import or @namespace rule will cause any subsequent @import or @namespace rules to be
       ignored."
   Together those are exactly `[@layer statement]* [@import]* [@namespace]* [anything]*`, and §6.4 step 5's
   "cannot be inserted ... due to constraints specified by CSS" is "the insertion would not match it".
   IT IS A SET OF ZONES PER RULE TYPE AND NOT A RANK, AND THE `@layer` STATEMENT IS WHY — it is the one type
   with TWO admissible positions, which a single rank per type cannot state. The comment this replaces
   PREDICTED that such a rule would simply take `@import`'s rank, and that was wrong in BOTH directions: a rank
   shared with `@import` would have REFUSED `@layer a;` after a style rule, which §6.4.4.2 allows outright, and
   ADMITTED one between an `@import` and an `@namespace`, which its note forbids. Stating the whole prologue is
   what keeps both directions falling out of one walk — a style rule BEFORE an `@import` is refused
   (css/cssom/insertRule-import-no-index.html) by the same walk that refuses an `@import` after one. */
enum { ZONE_LEAD = 0, ZONE_IMPORT, ZONE_NAMESPACE, ZONE_BODY, ZONE_N };
#define ZONE_BIT(z) (1u << (unsigned)(z))

/* §6.4.2's `type` for a rule whose interface is `type` — the frozen table's number, or its own "otherwise,
   return 0" for an interface the table does not name. */
static uint32_t rule_legacy_type(uint16_t type)
{
    if (type >= RULE_TYPE_UNNUMBERED) return 0;
    DCHECK(type >= RULE_TYPE_STYLE && type <= RULE_TYPE_SUPPORTS,
           "a CSS rule's interface discriminator is neither one of §6.4.2's table numbers nor above the end of "
           "the table — the enum above is the one place both halves are declared, so a value between them "
           "means a row was added without deciding which half it is in");
    return type;
}

typedef struct CssRuleData {
    JSValue parent_style_sheet;  /* §6.4.2 "parent CSS style sheet" (OWNED) */
    JSValue parent_rule;         /* §6.4.2 "parent CSS rule" (OWNED) */
    /* THE RULE'S PRELUDE, in the canonical form its own getter must answer — §6.4.3's selector list, §6.4.7's
       page selector list and CSS Animations §6.2.2's keyText are three grammars and one field, because each
       is that rule's prelude serialized and each is what its setter replaces. JS_NULL on a rule that has
       none. (OWNED) */
    JSValue selector_text;
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
       attribute's own null and not an absence this record has to distinguish from it. (OWNED)
       `supports_text` IS ALSO CSS Conditional §7.4's CONDITION, and that is one fact under two attribute
       names rather than two facts sharing a slot — the test `at_name` and `keyframes_name` failed one field
       up and this one passes. §6.4.4's `supportsText` is "the <supports-condition> declared in the at-rule"
       and §7.4's `conditionText` is the `<supports-condition>` an `@supports` rule's prelude IS: the same
       grammar (core/css/css_supports.h parses ONE production for both), the same evaluation, the same
       normalization rule (§7.4's "token stream simplifications are allowed ... logical simplifications are
       not" is what an `@import`'s raw span already is), and both readonly. The two rule types are disjoint,
       so no rule ever needs to answer both. */
    JSValue href;
    JSValue layer_name;
    JSValue supports_text;
    /* §6.4.9's two, JS_NULL on a rule that is not an `@namespace`. `prefix` is the EMPTY STRING for the
       default namespace — "the prefix ... or the empty string if there is no prefix" — so a JS_NULL here is
       only ever "this is not a namespace rule". (OWNED) */
    JSValue namespace_uri;
    JSValue prefix;
    /* THE AT-KEYWORD THE RULE WAS WRITTEN WITH — the `@` not included — on every rule type whose `type` does
       not determine it, and JS_NULL on every rule whose type does. Two types do not, and they are the same
       question asked of two closed sets rather than two facts sharing a slot:
         - §6.4.8's `name`, "the name of the margin at-rule. The @ character is not included in the name." Its
           `type` says MARGIN and nothing else on the record could say WHICH of CSS Paged Media §4.3's sixteen
           margin boxes, and `cssText` needs the same string the attribute returns.
         - A CSS Animations §6.3 keyframes rule's `@keyframes` or `@-webkit-keyframes`. CSS Compatibility
           Standard §3.1 "CSS At-rules" makes those two spellings of ONE rule — one interface, one prototype,
           one `type` of 7, one `<keyframes-name>` grammar — so `type` cannot say which the page wrote, and
           §6.4's serialization is where the difference shows. No IDL member returns it, which is the ONLY way
           the two rows differ and is not a difference in the fact.
       So it is one string with one meaning, read with the rule's type in hand at both sites — which is the
       test `keyframes_name` beside it FAILS (an author's case-sensitive settable `<custom-ident>` is not an
       at-keyword out of a closed set), and which the two `@layer` interfaces sharing `layer_names` pass. A
       margin rule's is never absent and a keyframes rule's is never absent, so this field has no third state
       for either of them to have to distinguish. */
    JSValue at_name;
    /* CSS Animations §6.3.2's `name` — "the name of the keyframes, used by the animation-name property".
       JS_NULL on every rule that is not a `@keyframes`. It is a SECOND name field beside `at_name` and not a
       widening of it, and a keyframes rule carrying BOTH is where that is easiest to see: `at_name` is the
       AT-KEYWORD, out of a closed set the standards fix, ASCII-lowercased by the parse and reachable by no IDL
       member, while this one is an author's `<custom-ident>` or `<string>`, FULLY case-sensitive ("two names
       are equal only if they are codepoint-by-codepoint equal"), settable through §6.3.2, and serialized by a
       rule of its own. One slot would have had to be read with the rule's type in hand at every site anyway,
       which is two facts wearing one name. */
    JSValue keyframes_name;
    /* CSS Cascade §8.1's `name` and §8.2's `nameList` — the `<layer-name>`s the `@layer` at-rule ITSELF
       declares, as the FROZEN Array §8.2's `FrozenArray<CSSOMString>` value IS (Web IDL §2.13.35: such a value
       is "a reference to an object that holds a fixed length array of unmodifiable values", so the freeze
       belongs to the stored value and not to the getter). JS_NULL on every rule that is not an `@layer`.
       ONE FIELD FOR TWO INTERFACES, unlike `at_name` and `keyframes_name` beside it, and the test is the one
       those two failed: those are two DIFFERENT facts under one word (a closed at-keyword out of CSS Paged
       Media's sixteen, against an author `<custom-ident>` that is case-sensitive and settable). These are the
       SAME fact under two multipliers — core/css/css_at_rule_prelude.h parses `<layer-name>?` and
       `<layer-name>#` with one grammar and normalizes both the same way, which is what §8.2 requires outright
       ("normalized following the same rule as the CSSLayerBlockRule's name attribute"). A block rule's list
       holds at most one entry and §8.1's `name` is that entry, or the empty string for §6.4.2.1's anonymous
       layer; a statement rule's holds one or more. Two fields could disagree about which. (OWNED) */
    JSValue layer_names;
    /* CSS Properties and Values API 1 §3's `<custom-property-name>#` prelude — the names the `@property`
       at-rule declares, as an Array. JS_NULL on every rule that is not an `@property`.
       IT IS A LIST WHERE §6.1 HAS ONE `name`, and that is the spec's own unfinished edge rather than a shape
       chosen here: §3's prelude carries a `#` multiplier and §3 says "a valid @property rule represents a
       custom property registration for EACH <custom-property-name> in the rule's prelude", while §6.1 declares
       one `readonly attribute CSSOMString name` and attaches the note "the CSSOM for multi-name @property rules
       has not been resolved on by the CSSWG [w3c/csswg-drafts Issue #14227]". So the RULE is what the prelude
       says and the ATTRIBUTE is what §6.1 says, and the getter is where the two meet — see its own crash. It is
       a field of its own and not `layer_names` beside it, because those are two different facts under one
       word: a `<layer-name>` is a dotted cascade-layer path that is serialized and case-preserved, and a
       `<custom-property-name>` is a `<dashed-ident>` that identifies a property. (OWNED) */
    JSValue property_names;
    /* §3.1's `syntax` descriptor, as the `<string>`'s own value — §3.1's INITIAL `"*"` when the rule declares
       none, and also when it declares one that is not a valid syntax string, which is that section's own
       sentence ("the descriptor is invalid and must be ignored") and not a fallback. JS_NULL on every rule that
       is not an `@property`. (OWNED) */
    JSValue property_syntax;
    /* §3.2's `inherits` descriptor — JS_TRUE or JS_FALSE, §3.2's INITIAL being `true`. JS_NULL on every rule
       that is not an `@property`, which is why it is a JSValue and not a C bool: a bool has no third state, and
       "this rule declares no inherit flag because it is not an @property" is a different fact from either
       flag. (OWNED) */
    JSValue property_inherits;
    /* §3.3's `initial-value` descriptor, as the `<declaration-value>` text it was declared with. JS_NULL is
       §3.3's INITIAL — the guaranteed-invalid value — which §6.1 answers as the null of its nullable
       `initialValue`, so on an `@property` this field's null IS the attribute's null; on any other rule it is
       "not an `@property`", and the getter's brand check is what decides which question was asked. That is the
       same doubling §6.4.4's `layerName` and `supportsText` carry, for the same reason. (OWNED) */
    JSValue property_initial_value;
    /* CSS Conditional 5 §9.1's `conditions` — the `FrozenArray<CSSContainerCondition>` an `@container` rule's
       prelude IS, as the Array that TYPE's values are references to (Web IDL §2.13.35, the same reading
       `layer_names` above is held to). Each entry is a frozen `{ name, query }`. JS_NULL on every rule that is
       not an `@container`.
       IT IS THE ONLY STORED FORM OF THE CONDITION, and that is what keeps §9.1's four members one fact: §9.1
       defines `containerName`, `containerQuery` AND §7.2's `conditionText` over this very list ("let conditions
       be the result of getting the conditions attribute"), so a second field holding a name or a joined text
       would be a second answer able to disagree with the list a page reads. It is not `layer_names` beside it
       for the reason `at_name` is not `keyframes_name`: a layer name list is a list of ONE kind of string, and
       this is a list of PAIRS whose halves obey different rules — the name is serialized and canonical, the
       query is the author's raw span that §9.1 forbids re-serializing. (OWNED) */
    JSValue container_conditions;
    uint16_t type;
} CssRuleData;

static JSClassID g_rule_class;
/* THE INTERFACE PROTOTYPES, each the REALM's — §3.7, and core/realm.h's slot store IS quickjs's own per-context
   slot array, freed with the context. Three of them are abstract (nothing is an instance of CSSRule,
   CSSGroupingRule or CSSConditionRule) and the rest are concrete; all are held the same way because a rule is
   built with an EXPLICIT prototype chosen from its type, so the class's own proto slot decides nothing. */
enum { PROTO_RULE = 0, PROTO_GROUPING, PROTO_STYLE, PROTO_CONDITION, PROTO_MEDIA, PROTO_SUPPORTS,
       PROTO_CONTAINER, PROTO_IMPORT, PROTO_NAMESPACE, PROTO_FONT_FACE, PROTO_PAGE, PROTO_MARGIN,
       PROTO_KEYFRAMES, PROTO_KEYFRAME, PROTO_LAYER_BLOCK, PROTO_LAYER_STATEMENT, PROTO_PROPERTY, PROTO_N };
static int g_proto_slot[PROTO_N];
static int g_id_set_selector = -1, g_id_set_page_selector = -1, g_id_set_key_text = -1,
           g_id_set_keyframes_name = -1, g_id_set_css_text = -1, g_id_insert_rule = -1, g_id_delete_rule = -1,
           g_id_append_rule = -1, g_id_kf_delete_rule = -1, g_id_find_rule = -1;

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
    (uint16_t)offsetof(CssRuleData, at_name),
    (uint16_t)offsetof(CssRuleData, keyframes_name),
    (uint16_t)offsetof(CssRuleData, layer_names),
    (uint16_t)offsetof(CssRuleData, property_names),
    (uint16_t)offsetof(CssRuleData, property_syntax),
    (uint16_t)offsetof(CssRuleData, property_inherits),
    (uint16_t)offsetof(CssRuleData, property_initial_value),
    (uint16_t)offsetof(CssRuleData, container_conditions),
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

/* WRITE ONE OF THE TWENTY-ONE, and never `JS_FreeValue(ctx, r->f); r->f = <build one>;` — see cow.h for the
   order and the defect. The DISCRIMINATOR is worth stating, because most of this file's refills are strings and
   look identical to the ones that bite: js_trigger_gc has exactly one caller, JS_NewObjectFromShape, so an
   OBJECT allocation is a collection and a string allocation is not. The refills that build an object —
   media_list_new, the three *_array builders, and the `style` factory — therefore run the collector with this
   record's slot naming freed storage, and rule_gc_mark walks that slot and decrefs a JSObject already back on
   the allocator's free list. A run of adjacent frees is the same hazard arriving the other way: the second
   release may run a finalizer that allocates, and the first slot has not been rewritten yet.
   Routing ALL of them rather than the object-valued ones is the point — which slot is a string today is not a
   property the next reader should have to re-derive to know whether a write is safe.
   The MINT does not come here: rule_new fills every slot before JS_SetOpaque, where the record is unreachable
   by the collector and has no value to release. */
static void rule_set(JSContext *ctx, CssRuleData *r, JSValue *slot, JSValue v)
{
    cow_record_set(ctx, r, &RULE_REC, slot, v);
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
    JS_FreeValueRT(rt, r->at_name);
    JS_FreeValueRT(rt, r->keyframes_name);
    JS_FreeValueRT(rt, r->layer_names);
    JS_FreeValueRT(rt, r->property_names);
    JS_FreeValueRT(rt, r->property_syntax);
    JS_FreeValueRT(rt, r->property_inherits);
    JS_FreeValueRT(rt, r->property_initial_value);
    JS_FreeValueRT(rt, r->container_conditions);
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
    JS_MarkValue(rt, r->at_name, mark_func);
    JS_MarkValue(rt, r->keyframes_name, mark_func);
    JS_MarkValue(rt, r->layer_names, mark_func);
    JS_MarkValue(rt, r->property_names, mark_func);
    JS_MarkValue(rt, r->property_syntax, mark_func);
    JS_MarkValue(rt, r->property_inherits, mark_func);
    JS_MarkValue(rt, r->property_initial_value, mark_func);
    JS_MarkValue(rt, r->container_conditions, mark_func);
}

/* ---- §6.4's CSS RULE LIST, as INFRA's list operations over an Array ---------------------------------------- */

/* AN ARRAY'S `length`, which is INFRA's SIZE over one. It is not named for rule lists any more: a `@layer`
   at-rule's `<layer-name>` list is an Array on the record too, and one length reader is what stops the two
   collections growing two implementations that could disagree. */
static uint32_t array_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

static void rules_insert_at(JSContext *ctx, JSValueConst list, uint32_t i, JSValue v)   /* CONSUMES v */
{
    uint32_t n = array_len(ctx, list), k;

    DCHECK(i <= n, "§6.4 inserted a CSS rule at an index past the end of the list");
    for (k = n; k > i; k--)
        JS_SetPropertyUint32(ctx, list, k, JS_GetPropertyUint32(ctx, list, k - 1));
    JS_SetPropertyUint32(ctx, list, i, v);
}

static void rules_remove_at(JSContext *ctx, JSValueConst list, uint32_t i)
{
    uint32_t n = array_len(ctx, list), k;

    DCHECK(i < n, "§6.4 removed a CSS rule at an index the list does not have");
    for (k = i + 1; k < n; k++)
        JS_SetPropertyUint32(ctx, list, k - 1, JS_GetPropertyUint32(ctx, list, k));
    JS_SetPropertyStr(ctx, list, "length", JS_NewUint32(ctx, n - 1));
}

/* DOES THIS RULE TYPE HOLD CHILD RULES AT ALL — which decides whether the create allocates a list, whether the
   parse may put a rule inside one, and whether the serialization has children to walk.
   §6.4.7 declares `interface CSSPageRule : CSSGroupingRule`, and that is not a formality: an `@page` CONTAINS
   CSS Paged Media §4.3's margin at-rules, so `pageRule.cssRules[0]` is a CSSMarginRule. §6.4.8's CSSMarginRule
   is NOT one — its body is a `<declaration-list>` and holds no rules at all, and neither does a
   `<keyframe-block>`'s. */
static bool rule_type_has_child_rules(uint16_t type)
{
    return type == RULE_TYPE_STYLE || type == RULE_TYPE_MEDIA || type == RULE_TYPE_SUPPORTS ||
           type == RULE_TYPE_CONTAINER || type == RULE_TYPE_PAGE || type == RULE_TYPE_KEYFRAMES ||
           type == RULE_TYPE_LAYER_BLOCK;
}

/* IS THIS RULE TYPE A §6.4.5 GROUPING RULE — "an at-rule that CONTAINS OTHER RULES nested inside itself", plus
   the style rule CSS Nesting made one. It is Web IDL §3.7.5's brand check for `cssRules`, `insertRule` and
   `deleteRule` AS CSSGroupingRule DECLARES THEM: a page can reach `CSSGroupingRule.prototype.insertRule` and
   apply it to an `@import` rule, and the answer is a TypeError and not an insertion into a list that rule does
   not have.
   IT IS A NARROWER QUESTION THAN THE ONE ABOVE, and CSS Animations is why. Its CSSKeyframesRule holds child
   rules and is `interface CSSKeyframesRule : CSSRule` — it does not inherit CSSGroupingRule and declares its
   OWN `cssRules`, `appendRule(CSSOMString)` and `deleteRule(CSSOMString)`, whose argument is a keyframe
   SELECTOR where §6.4.5's is an index. Answering both questions from one predicate would have put §6.4.5's
   index-taking `deleteRule` on a `@keyframes` beside the selector-taking one it really has.
   CSS Cascade §8.1's CSSLayerBlockRule answers YES to BOTH, and that is stated twice over rather than assumed:
   its IDL is `interface CSSLayerBlockRule : CSSGroupingRule`, and §6.4.4.1 gives the reason behind the IDL —
   "such @layer block rules have the same restrictions and processing as a conditional group rule
   [CSS-CONDITIONAL-3] with a true condition". A CSSConditionRule it is NOT: a layer has no condition, so
   §7.2's `conditionText` is not on it and this predicate is not that one. */
static bool rule_type_is_grouping(uint16_t type)
{
    bool grouping = type == RULE_TYPE_STYLE || type == RULE_TYPE_MEDIA || type == RULE_TYPE_SUPPORTS ||
                    type == RULE_TYPE_CONTAINER || type == RULE_TYPE_PAGE || type == RULE_TYPE_LAYER_BLOCK;

    DCHECK(!grouping || rule_type_has_child_rules(type),
           "a rule type is a §6.4.5 grouping rule and yet holds no child rules — CSSGroupingRule is DEFINED as "
           "a rule that contains other rules, so the two tables above have drifted apart");
    return grouping;
}

/* §6.4.5's CHILD CSS RULES — the very Array a grouping rule's `cssRules` shares. OWNED. */
static JSValue rule_child_rules(JSContext *ctx, JSValueConst rule)
{
    CssRuleData *r = rule_of(rule);

    DCHECK(r != NULL, "a rule's child CSS rules were read off something that is not a CSS rule");
    DCHECK(rule_type_has_child_rules(r->type),
           "a rule that holds no child rules was asked for its child CSS rules. An `@import`, an `@namespace`, "
           "an `@font-face`, a §4.3 margin at-rule and a `<keyframe-block>` contain no rules at all, so the "
           "answer is not an empty list — it is that the question does not apply, and every member that could "
           "ask it is brand-checked");
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
    /* Only a rule that HAS child CSS rules gets one — an empty Array on an `@import` would read as a list that
       happens to be empty, which is a different fact. */
    r->child_rules = rule_type_has_child_rules(type) ? JS_NewArray(ctx) : JS_NULL;
    CHECK(!JS_IsException(r->child_rules), "a CSS rule's child list could not be allocated");
    r->rule_list = JS_UNDEFINED;
    r->media = JS_NULL;
    r->href = JS_NULL;
    r->layer_name = JS_NULL;
    r->supports_text = JS_NULL;
    r->namespace_uri = JS_NULL;
    r->prefix = JS_NULL;
    r->at_name = JS_NULL;
    r->keyframes_name = JS_NULL;
    r->layer_names = JS_NULL;
    r->property_names = JS_NULL;
    r->property_syntax = JS_NULL;
    r->property_inherits = JS_NULL;
    r->property_initial_value = JS_NULL;
    r->container_conditions = JS_NULL;
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
    rule_set(ctx, r, &r->selector_text, JS_NewString(ctx, selector_text));
    rule_set(ctx, r, &r->block_text, JS_NewString(ctx, block_text));
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
    rule_set(ctx, r, &r->media, media_list_new(ctx, prelude));
    return obj;
}

/* IS THIS RULE'S FEATURE QUERY TRUE — CSS Conditional §6, over the condition the rule stores. Read by §7.4's
   `matches` and by the author cascade, which are the same question asked for two purposes.
   IT IS RECOMPUTED RATHER THAN CACHED, and that is the `@media` arm's own arrangement one function down: a
   media rule builds its `MediaQuerySet` from stored text at every evaluation too. A cached boolean would be a
   17th field on a record whose every field is an obligation at the clone, the finalizer, the gc_mark and the
   COW layout — for a value that is a pure function of a string this record already holds and that therefore
   cannot disagree with itself however often it is derived. */
static bool rule_supports_matches(JSContext *ctx, CssRuleData *r)
{
    const char *text;
    bool matched = false, valid;

    DCHECK(r->type == RULE_TYPE_SUPPORTS,
           "a rule that is not an `@supports` was asked whether its feature query holds");
    DCHECK(JS_IsString(r->supports_text),
           "an `@supports` rule holds no condition text — its prelude IS a `<supports-condition>`, and a rule "
           "whose prelude did not parse as one is never built (CSS Conditional §6: processors must ignore "
           "such a rule, including all of its contents)");
    text = JS_ToCString(ctx, r->supports_text);
    if (!text) return false;
    valid = css_supports_condition(text, strlen(text), &matched);
    JS_FreeCString(ctx, text);
    DCHECK(valid,
           "an `@supports` rule's STORED condition no longer parses as a `<supports-condition>` — the builder "
           "refuses a prelude that does not, and nothing may write this field afterwards (§7.2's "
           "`conditionText` is readonly), so the two readings of one text have disagreed");
    return valid && matched;
}

/* A CSS Conditional §7.4 CSSSupportsRule over the `@supports` rule's own prelude, which IS the
   `<supports-condition>` §6 defines. JS_UNDEFINED — the builder's drop — when that prelude matches no
   production of the grammar, which is §6's own disposal: "Any @supports rule that does not parse according to
   the grammar above ... is invalid. Style sheets must not use such a rule and processors must ignore such a
   rule (including all of its contents)."
   THE PRELUDE IS STORED AS WRITTEN, unlike `@media`'s, and §7.4 is why: `conditionText` "must return the
   condition that was specified, WITHOUT ANY LOGICAL SIMPLIFICATIONS, so that the returned condition will
   evaluate to the same result as the specified condition in any conformant implementation ... including
   implementations that implement future extensions allowed by the <general-enclosed> extensibility
   mechanism". A round trip through this engine's parse would drop precisely what `<general-enclosed>` exists
   to carry — the constructs THIS build does not understand and a future one will — so there is nothing to
   canonicalise it through. §7.3's is the opposite instruction ("must return the value of media.mediaText"),
   which is why the two conditional rules store their conditions differently. */
static JSValue supports_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                                 const char *prelude)
{
    JSValue obj;
    CssRuleData *r;
    bool matched = false;

    DCHECK(prelude != NULL, "a CSSSupportsRule was built with no prelude — `@supports {}` has an EMPTY "
                            "condition, which matches no production of §6's grammar and is a DROP, and the "
                            "absence of one is a parse that never reported the prelude at all");
    if (!css_supports_condition(prelude, strlen(prelude), &matched)) return JS_UNDEFINED;
    obj = rule_new(ctx, PROTO_SUPPORTS, RULE_TYPE_SUPPORTS, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) return obj;
    r = JS_GetOpaque(obj, g_rule_class);
    rule_set(ctx, r, &r->supports_text, JS_NewString(ctx, prelude));
    return obj;
}

/* THE CONDITIONS AN `@container` AT-RULE DECLARES, as the record's own FLAT Array — `[name0, query0, name1,
   query1, …]`, frozen.
   IT IS FLAT AND NOT AN ARRAY OF `{name, query}` OBJECTS, WHICH LOOKS LIKE THE IDL VALUE AND IS NOT ONE.
   §9.1's `conditions` is a `FrozenArray<CSSContainerCondition>` and is NOT `[SameObject]`, and its algorithm
   says what that means in full: "Let result be an empty list … Append dict to result. Return result" — a NEW
   list of NEW dictionary objects on every get. Storing the IDL value and handing the same one back would make
   `rule.conditions === rule.conditions` answer true where the platform answers false, and — worse — would let
   a page write `rule.conditions[0].name` into the rule's own record, because Web IDL §3.2.27's freeze is
   `SetIntegrityLevel(array, "frozen")` on the ARRAY and says nothing about the dictionaries inside it. So the
   record holds the FACTS and the one member that returns dictionaries mints them, which is also why the three
   readers that do NOT return dictionaries (`containerName`, `containerQuery`, §7.2's `conditionText`) read the
   pairs directly rather than through a list they would only take apart again.
   The freeze is still applied here, for the reason `layer_names_array` gives one level up: it is the record's
   value and nothing in this build may write it. */
static JSValue container_conditions_array(JSContext *ctx, const CssContainerConditions *c)
{
    JSValue a = JS_NewArray(ctx);
    unsigned i;

    CHECK(!JS_IsException(a), "cssom: an `@container` rule's condition list could not be allocated");
    for (i = 0; i < c->n; i++) {
        DCHECK(c->v[i].name != NULL && c->v[i].query != NULL,
               "an `@container` rule's parsed conditions hold a NULL where §9.1's `name` or `query` belongs — "
               "the one parser writes the EMPTY STRING for a term the condition omits, which is §9.1's own "
               "answer and a different fact from an absent one");
        JS_SetPropertyUint32(ctx, a, 2 * i, JS_NewString(ctx, c->v[i].name));
        JS_SetPropertyUint32(ctx, a, 2 * i + 1, JS_NewString(ctx, c->v[i].query));
    }
    CHECK(idl_freeze_array(ctx, a) == 0, "cssom: an `@container` rule's condition list could not be frozen");
    return a;
}

/* A CSS Conditional 5 §9.1 CSSContainerRule over the `@container` at-rule's own prelude, which IS §5.4's
   `<container-condition>#`. JS_UNDEFINED — the builder's drop — when that prelude matches no production, which
   is CSS Syntax §8 "CSS stylesheets"'s disposal for an at-rule "invalid according to its grammar": discard the
   rule, contents included, exactly as `@supports display:flex {}` is discarded one function up. */
static JSValue container_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                                  const char *prelude)
{
    CssContainerConditions conds = { NULL, 0 };
    JSValue obj;
    CssRuleData *r;

    DCHECK(prelude != NULL,
           "a CSSContainerRule was built with no prelude — `@container {}` has an EMPTY one, which §5.4's `!` "
           "refuses and which is therefore a DROP, and the ABSENCE of one is a parse that never reported the "
           "prelude at all");
    if (!css_prelude_container_conditions(prelude, strlen(prelude), &conds)) return JS_UNDEFINED;
    obj = rule_new(ctx, PROTO_CONTAINER, RULE_TYPE_CONTAINER, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) { css_container_conditions_free(&conds); return obj; }
    r = JS_GetOpaque(obj, g_rule_class);
    rule_set(ctx, r, &r->container_conditions, container_conditions_array(ctx, &conds));
    css_container_conditions_free(&conds);
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
    rule_set(ctx, r, &r->href, JS_NewString(ctx, p.href));
    if (p.layer_name) rule_set(ctx, r, &r->layer_name, JS_NewString(ctx, p.layer_name));
    if (p.supports_text) {
        rule_set(ctx, r, &r->supports_text, JS_NewString(ctx, p.supports_text));
    }
    rule_set(ctx, r, &r->media, media_list_new(ctx, p.media_text));
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
    rule_set(ctx, r, &r->prefix, JS_NewString(ctx, prefix));
    rule_set(ctx, r, &r->namespace_uri, JS_NewString(ctx, uri));
    free(prefix);
    free(uri);
    return obj;
}

/* A CSS Fonts 5 §9.1 CSSFontFaceRule over the DESCRIPTORS its block declares. They are kept in `block_text`,
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
    rule_set(ctx, r, &r->block_text, JS_NewString(ctx, block_text));
    return obj;
}

/* WHICH RULE'S BLOCK THIS RULE'S DECLARATIONS ARE — the one statement of it, read by the three creators whose
   bodies are restricted and by §6.6.1's write path, so a rule type cannot be filtered at the parse and left
   unfiltered at a `setProperty` (or the reverse), which is a `length` that disagrees with its own `cssText`.
   A rule with NO declaration block at all never reaches this: `css_rule_set_block_text` asserts which five
   types have one, and each creator calls it about itself. */
static CssomBlockContext rule_block_context(uint16_t type)
{
    if (type == RULE_TYPE_PAGE) return CSSOM_BLOCK_PAGE;
    if (type == RULE_TYPE_MARGIN) return CSSOM_BLOCK_MARGIN;
    if (type == RULE_TYPE_KEYFRAME) return CSSOM_BLOCK_KEYFRAME;
    DCHECK(type == RULE_TYPE_STYLE || type == RULE_TYPE_FONT_FACE,
           "a rule type that has no declaration block was asked which restriction its block carries — §6.4.3's "
           "CSSStyleRule and CSS Fonts 5 §9.1's CSSFontFaceRule are the two whose blocks are unrestricted, and "
           "every other rule with a block is named above");
    return CSSOM_BLOCK_UNRESTRICTED;
}

/* A §6.4.7 CSSPageRule over the `@page` at-rule's prelude and its page descriptors. Both halves are grammars
   this file does not own: CSS Paged Media §4.3's `<page-selector-list>` is parsed and canonicalised by
   core/css/css_at_rule_prelude.h (a prelude whose grammar FAILS is not an `@page` rule at all — CSS Syntax
   drops it, which is JS_UNDEFINED here and the same answer `@namespace a b c;` gets), and §4.3's restriction
   on WHICH declarations a page context holds is applied by core/css/css_style_declaration.h before the text is
   stored, so every reader of the block sees the declarations the rule really has. The child list an `@page`
   gets from `rule_new` is where its margin at-rules go. */
static JSValue page_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                             const char *prelude, const char *block_text)
{
    char *selectors, *decls;
    JSValue obj;
    CssRuleData *r;

    DCHECK(prelude != NULL && block_text != NULL,
           "a CSSPageRule was built without both of the texts it IS — `@page {}` has an EMPTY page selector "
           "list and declares NOTHING, which are two empty strings and not two absences");
    selectors = css_prelude_page_selectors(prelude, strlen(prelude));
    if (!selectors) return JS_UNDEFINED;
    obj = rule_new(ctx, PROTO_PAGE, RULE_TYPE_PAGE, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) { free(selectors); return obj; }
    r = JS_GetOpaque(obj, g_rule_class);
    rule_set(ctx, r, &r->selector_text, JS_NewString(ctx, selectors));
    free(selectors);
    decls = cssom_serialize_declarations(block_text, strlen(block_text), CSSOM_BLOCK_PAGE);
    rule_set(ctx, r, &r->block_text, JS_NewString(ctx, decls ? decls : ""));
    free(decls);
    return obj;
}

/* A §6.4.8 CSSMarginRule. `name` is the at-keyword with no `@`, which the parse has already ASCII-lowercased
   (an at-keyword is case-insensitive, so `@TOP-LEFT` is `@top-left`), and the body is a `<declaration-list>`
   of page-margin properties — the margin context's own list, which is not the page context's. */
static JSValue margin_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                               const char *name, const char *block_text)
{
    char *decls;
    JSValue obj;
    CssRuleData *r;

    DCHECK(name != NULL && css_page_margin_at_rule(name),
           "a CSSMarginRule was built for an at-rule that is not one of CSS Paged Media §4.3's sixteen margin "
           "at-rules — the builder decides that from the name before it gets here, because outside an `@page` "
           "the very same name is an unknown at-rule that CSS Syntax drops");
    DCHECK(block_text != NULL,
           "a CSSMarginRule was built with no declaration text — `@top-left {}` declares NOTHING, which is the "
           "empty string, and the absence of one is a parse that did not report the block at all");
    obj = rule_new(ctx, PROTO_MARGIN, RULE_TYPE_MARGIN, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) return obj;
    r = JS_GetOpaque(obj, g_rule_class);
    rule_set(ctx, r, &r->at_name, JS_NewString(ctx, name));
    decls = cssom_serialize_declarations(block_text, strlen(block_text), CSSOM_BLOCK_MARGIN);
    rule_set(ctx, r, &r->block_text, JS_NewString(ctx, decls ? decls : ""));
    free(decls);
    return obj;
}

/* THE PREFIXED SPELLINGS OF AT-RULES THIS BUILD ALREADY HAS — CSS Compatibility Standard §3.1 "CSS At-rules",
 * ENTIRE: "The following -webkit- vendor prefixed at-rules must be supported as aliases of the corresponding
 * unprefixed at-rules", over a table whose every row is transcribed below. It has ONE row today, and that is
 * the table's own state and not a subset chosen here: a row is added when that section adds one.
 *
 * AN ALIAS IS A SECOND SPELLING OF ONE RULE, NOT A SECOND RULE. §3.1 aliases the AT-KEYWORD, so everything
 * downstream of the name is the unprefixed rule's: the same `<keyframes-name>` grammar (so
 * `@-webkit-keyframes none {}` is dropped exactly as `@keyframes none {}` is), the same CSS Animations §6.3
 * CSSKeyframesRule interface and prototype, the same §6.4.2 `type` of 7, the same body. That is why this is a
 * NAME RESOLUTION in front of the builder's dispatch rather than an arm inside it — an arm would be a second
 * creator able to disagree with the first about any of those, and the disagreement would be invisible.
 *
 * WHAT THE ALIAS DOES CHANGE IS THE SERIALIZATION, AND THE SPEC IS SILENT THERE. CSSOM §6.4 "CSS Rules"'s
 * serialize-a-CSS-rule CSSKeyframesRule arm opens with "The literal string "@keyframes ", followed by a single
 * SPACE" — an arm written before §3.1 existed and which does not mention the second spelling. Both readings
 * are defensible from the text alone, so the question is settled by MEASUREMENT: real Chrome 148.0.7778.167
 * answers `@-webkit-keyframes spin { \n  0% { … }\n}` for a rule written with the prefix and `@keyframes …`
 * for one written without, off one CSSKeyframesRule interface with `type` 7 in both cases. So the rule carries
 * the AT-KEYWORD IT WAS WRITTEN WITH and the arm emits that, which is also the only reading under which
 * `cssText` re-parses to the rule it came from.
 *
 * THE TABLE IS ASSERTED AGAINST ITS OWN SECTION on every lookup, because a row is a claim about §3.1 and a
 * wrong one reads exactly like a right one: §3.1's table is "-webkit- prefixed at-rule alias" against "the
 * CORRESPONDING unprefixed at-rule", so the two names in a row are one at-keyword with and without the
 * prefix, and a row that is not is a row nobody read that section for. */
static const struct { const char *prefixed; const char *unprefixed; } AT_RULE_ALIASES[] = {
    { "-webkit-keyframes", "keyframes" },
};

/* The unprefixed at-keyword §3.1 aliases `name` onto, or NULL when it aliases none. `name` is the at-keyword
   with no `@`, ASCII-lowercased by the parse — which is what makes `@-WEBKIT-KEYFRAMES` this row too. */
static const char *at_rule_alias(const char *name)
{
    static const char PREFIX[] = "-webkit-";
    const size_t plen = sizeof PREFIX - 1;
    unsigned i;

    DCHECK(name != NULL, "an at-keyword was looked up in CSS Compatibility §3.1's alias table with no name");
    for (i = 0; i < sizeof AT_RULE_ALIASES / sizeof AT_RULE_ALIASES[0]; i++) {
        DCHECK(strncmp(AT_RULE_ALIASES[i].prefixed, PREFIX, plen) == 0 &&
               strcmp(AT_RULE_ALIASES[i].prefixed + plen, AT_RULE_ALIASES[i].unprefixed) == 0,
               "a row of CSS Compatibility Standard §3.1 \"CSS At-rules\"'s alias table pairs two at-keywords "
               "that are not one name with and without the `-webkit-` prefix. §3.1 aliases a PREFIXED at-rule "
               "onto THE CORRESPONDING UNPREFIXED one, so a row spelling anything else is a row that was "
               "written from memory rather than read from that section, and it would silently reroute a "
               "page's rule to an interface the standard never named");
        if (strcmp(AT_RULE_ALIASES[i].prefixed, name) == 0) return AT_RULE_ALIASES[i].unprefixed;
    }
    return NULL;
}

/* A CSS Animations §6.3 CSSKeyframesRule over the `@keyframes` at-rule's prelude. That prelude is §3's
   `<keyframes-name>`, a grammar this file does not own (core/css/css_at_rule_prelude.h) and one whose failure
   is not a rule at all: `@keyframes none {}` and `@keyframes {}` are at-rules whose grammar failed, which CSS
   Syntax drops and which is JS_UNDEFINED here, the same answer `@namespace a b c;` gets.
   THE NAME IS STORED RAW, not serialized. §6.3.2's `name` returns what the author wrote (`@keyframes "foo"` is
   the name `foo`, and §6.3.2's setter stores whatever it is given), while §6.4's CSSKeyframesRule arm decides
   per read whether that name serializes as an identifier or as a string — two answers off one storage, which
   is why the storage is the one the attribute returns.
   `at_keyword` IS THE SPELLING THE PAGE WROTE — `keyframes`, or one CSS Compatibility Standard §3.1 "CSS
   At-rules" aliases onto it — and it is stored because it is the ONE thing the two spellings do not share:
   the interface, the prototype, the §6.4.2 `type`, the `<keyframes-name>` grammar and the body are the same
   rule, and only the at-keyword the serialization emits differs. See the alias table above for the
   measurement that says so.
   The child list `rule_new` gives it is where its `<keyframe-block>`s go. */
static JSValue keyframes_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                                  const char *at_keyword, const char *prelude)
{
    char *name;
    JSValue obj;
    CssRuleData *r;

    DCHECK(prelude != NULL, "a CSSKeyframesRule was built with no prelude — `<keyframes-name>` is REQUIRED by "
                            "§3's grammar, so an empty prelude is a rule that does not match it rather than a "
                            "rule with an empty name");
    DCHECK(at_keyword != NULL &&
               (strcmp(at_keyword, "keyframes") == 0 ||
                (at_rule_alias(at_keyword) != NULL &&
                 strcmp(at_rule_alias(at_keyword), "keyframes") == 0)),
           "a CSS Animations §6.3 CSSKeyframesRule was built for an at-keyword that is neither `@keyframes` "
           "nor one CSS Compatibility Standard §3.1 \"CSS At-rules\" aliases onto it. §3.1 aliases a prefixed "
           "spelling onto an EXISTING at-rule, so a row whose target is some other rule builds an object of "
           "the WRONG KIND wearing this interface's prototype — and the tell would be `cssText`, which would "
           "then emit an at-keyword that does not re-parse to the rule it came from");
    name = css_prelude_keyframes_name(prelude, strlen(prelude));
    if (!name) return JS_UNDEFINED;
    obj = rule_new(ctx, PROTO_KEYFRAMES, RULE_TYPE_KEYFRAMES, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) { free(name); return obj; }
    r = JS_GetOpaque(obj, g_rule_class);
    rule_set(ctx, r, &r->keyframes_name, JS_NewString(ctx, name));
    rule_set(ctx, r, &r->at_name, JS_NewString(ctx, at_keyword));
    free(name);
    return obj;
}

/* A CSS Animations §6.2 CSSKeyframeRule — one `<keyframe-block>`. Its prelude is §3's `<keyframe-selector>#`,
   canonicalised to §6.2.2's comma-separated percentages by core/css/css_at_rule_prelude.h, and its body is a
   `<declaration-list>` restricted by §3's own sentence (core/css/css_keyframes.h), applied here so that every
   reader of the block sees the declarations the rule really has.
   The prelude is kept in `selector_text` because that is what it IS: §6.2.2 calls it "the keyframe selector",
   and the field already carries §6.4.3's selector list and §6.4.7's page selector list for the same reason —
   it is the rule's prelude in the canonical form the getter must answer. */
static JSValue keyframe_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                                 const char *prelude, const char *block_text)
{
    char *keys, *decls;
    JSValue obj;
    CssRuleData *r;

    DCHECK(prelude != NULL && block_text != NULL,
           "a CSSKeyframeRule was built without both of the texts it IS — `0% {}` declares NOTHING, which is "
           "the empty string, and a block with NO prelude matches no `<keyframe-selector>#`");
    keys = css_prelude_keyframe_selectors(prelude, strlen(prelude));
    if (!keys) return JS_UNDEFINED;
    obj = rule_new(ctx, PROTO_KEYFRAME, RULE_TYPE_KEYFRAME, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) { free(keys); return obj; }
    r = JS_GetOpaque(obj, g_rule_class);
    rule_set(ctx, r, &r->selector_text, JS_NewString(ctx, keys));
    free(keys);
    decls = cssom_serialize_declarations(block_text, strlen(block_text), CSSOM_BLOCK_KEYFRAME);
    rule_set(ctx, r, &r->block_text, JS_NewString(ctx, decls ? decls : ""));
    free(decls);
    return obj;
}

/* THE `<layer-name>`s AN `@layer` AT-RULE DECLARES, as the frozen Array both interfaces answer from. The freeze
   is Web IDL §3.2.27's create-a-frozen-array ("perform SetIntegrityLevel(array, "frozen")") and it is applied
   HERE, once, because §8.2 types `nameList` a `FrozenArray<CSSOMString>` and §2.13.35 makes that type's values
   REFERENCES to a frozen object — so the frozen array is the value the record holds, which is also what makes
   `rule.nameList === rule.nameList` answer the way a reference does. */
static JSValue layer_names_array(JSContext *ctx, const CssLayerNames *names)
{
    JSValue a = JS_NewArray(ctx);
    unsigned i;

    CHECK(!JS_IsException(a), "cssom: an `@layer` rule's layer name list could not be allocated");
    for (i = 0; i < names->n; i++) {
        DCHECK(names->v[i] != NULL,
               "an `@layer` rule's parsed name list holds nothing where a `<layer-name>` belongs — the one "
               "parser appends a serialized name per entry and asserts that it is non-empty");
        JS_SetPropertyUint32(ctx, a, i, JS_NewString(ctx, names->v[i]));
    }
    CHECK(idl_freeze_array(ctx, a) == 0, "cssom: an `@layer` rule's layer name list could not be frozen");
    return a;
}

/* A CSS Cascade §8.1 CSSLayerBlockRule over the `@layer` BLOCK at-rule's prelude. §6.4.4.1's grammar is
   `@layer <layer-name>? { <rule-list> }` — AT MOST ONE name — so a prelude carrying a list is an at-rule whose
   grammar failed, which CSS Syntax drops and which is JS_UNDEFINED here, the same answer `@keyframes none {}`
   gets. The EMPTY list is the other outcome and it IS a rule: §6.4.2.1's anonymous layer, whose `name` §8.1
   states as the empty string and whose every occurrence is a layer of its own ("multiple unnamed layer rules
   place their styles into separate layers, as each occurrence is referencing a distinct anonymous layer name").
   The child list `rule_new` gives it is where the layer's rules go. */
static JSValue layer_block_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                                    const char *prelude)
{
    CssLayerNames names = { NULL, 0 };
    JSValue obj;
    CssRuleData *r;

    DCHECK(prelude != NULL,
           "a CSSLayerBlockRule was built with no prelude — `@layer { }` declares the ANONYMOUS layer, which is "
           "an EMPTY `<layer-name>?` and not the absence of a prelude");
    if (!css_prelude_layer_names(prelude, strlen(prelude), &names)) return JS_UNDEFINED;
    if (names.n > 1) { css_layer_names_free(&names); return JS_UNDEFINED; }
    obj = rule_new(ctx, PROTO_LAYER_BLOCK, RULE_TYPE_LAYER_BLOCK, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) { css_layer_names_free(&names); return obj; }
    r = JS_GetOpaque(obj, g_rule_class);
    rule_set(ctx, r, &r->layer_names, layer_names_array(ctx, &names));
    css_layer_names_free(&names);
    return obj;
}

/* A CSS Cascade §8.2 CSSLayerStatementRule over the `@layer` STATEMENT at-rule's prelude. §6.4.4.2's grammar is
   `@layer <layer-name>#;` — ONE OR MORE names, "unlike the block syntax, multiple comma-separated layer names
   can be provided in this syntax, declaring each of the layers in the order specified" — so the `#` multiplier
   has no zero-length arm and `@layer ;` is an at-rule whose grammar failed. That is the ONE thing this creator
   and the block's disagree about, which is why they share the grammar and not the multiplicity.
   It gets NO child list: a statement at-rule has no block at all, so it contains no rules and its `cssRules`
   is not merely empty but absent — §8.2 declares `interface CSSLayerStatementRule : CSSRule`. */
static JSValue layer_statement_rule_new(JSContext *ctx, JSValueConst parent_style_sheet,
                                        JSValueConst parent_rule, const char *prelude)
{
    CssLayerNames names = { NULL, 0 };
    JSValue obj;
    CssRuleData *r;

    DCHECK(prelude != NULL, "a CSSLayerStatementRule was built with no prelude");
    if (!css_prelude_layer_names(prelude, strlen(prelude), &names)) return JS_UNDEFINED;
    if (names.n == 0) { css_layer_names_free(&names); return JS_UNDEFINED; }
    obj = rule_new(ctx, PROTO_LAYER_STATEMENT, RULE_TYPE_LAYER_STATEMENT, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) { css_layer_names_free(&names); return obj; }
    r = JS_GetOpaque(obj, g_rule_class);
    rule_set(ctx, r, &r->layer_names, layer_names_array(ctx, &names));
    css_layer_names_free(&names);
    return obj;
}

/* THE `<custom-property-name>`s AN `@property` AT-RULE DECLARES, as the Array the record holds. It is an Array
   for the reason every other collection on this record is one — it has to park to the IDB cold tier and fork per
   flow, which a malloc'd list of pointers cannot (css_rule.h). It is NOT frozen the way CSS Cascade §8.2's
   `nameList` is: the freeze there is Web IDL §2.13.35's, which belongs to a `FrozenArray<T>` VALUE a page holds,
   and §6.1 hands no list to a page at all. */
static JSValue property_names_array(JSContext *ctx, const CssPropertyNames *names)
{
    JSValue a = JS_NewArray(ctx);
    unsigned i;

    CHECK(!JS_IsException(a), "cssom: an `@property` rule's custom property name list could not be allocated");
    DCHECK(names->n >= 1,
           "an `@property` rule's parsed prelude carries NO name — §3's `<custom-property-name>#` has no "
           "zero-length arm, so its parse answers false rather than handing back an empty list");
    for (i = 0; i < names->n; i++) {
        DCHECK(names->v[i] != NULL && names->v[i][0] == '-' && names->v[i][1] == '-',
               "an `@property` rule's parsed prelude holds something that is not a `<custom-property-name>` — "
               "CSS Variables §2 makes one a `<dashed-ident>`, and the one parser refuses anything else");
        JS_SetPropertyUint32(ctx, a, i, JS_NewString(ctx, names->v[i]));
    }
    return a;
}

/* A CSS Properties and Values API 1 §6.1 CSSPropertyRule over the `@property` at-rule's prelude and its
 * descriptor body.
 *
 * IT IS NOT A DECLARATION-BLOCK RULE, and §6.1's IDL is what says so: `interface CSSPropertyRule : CSSRule` with
 * four readonly attributes and NO `style`. So the body's declarations are not STORED as this rule's block the
 * way an `@font-face`'s are — there would be no member to read them back through, and `rule_block_context` would
 * have had to answer a question §6.1 never asks. They are read ONCE, here, into the three fields the three
 * descriptor attributes answer from, which is also what makes §6.1's serialization a walk over those fields
 * rather than over a declaration block whose order the author chose (the spec's arm emits `syntax`, `inherits`
 * and then `initial-value` whatever order they were written in, which `@property --valid-reverse` is exactly
 * the case for).
 *
 * EVERY DESCRIPTOR IS OPTIONAL AND EVERY ONE HAS AN INITIAL, which is §3's own sentence — "while the
 * <custom-property-name> is required, all of the descriptors are optional; when omitted, it matches the
 * behavior of an unregistered custom property" — with §3.1's `Initial: "*"`, §3.2's `Initial: true` and §3.3's
 * `Initial: the guaranteed-invalid value`. A descriptor whose VALUE does not match its own grammar is IGNORED
 * and takes that initial, and an unknown descriptor is ignored too: §3 says both, and adds the half that
 * matters most here — "unknown descriptors are invalid and ignored, BUT DO NOT INVALIDATE the @property rule".
 * So nothing in this body can drop the rule, and the only thing that can is the prelude. */
static JSValue property_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                                 const char *prelude, const char *block_text)
{
    CssPropertyNames names = { NULL, 0 };
    CssSyntaxDefinition def = { NULL, 0, false };
    JSValue obj;
    CssRuleData *r;
    size_t bl;
    char *declared, *syntax = NULL;
    bool inherits = true, star;

    DCHECK(prelude != NULL && block_text != NULL,
           "a CSSPropertyRule was built without both of the texts it IS — `@property --x {}` declares NOTHING, "
           "which is the empty string, and a prelude that is absent rather than empty is a parse that never "
           "reported one");
    if (!css_prelude_property_names(prelude, strlen(prelude), &names)) return JS_UNDEFINED;
    obj = rule_new(ctx, PROTO_PROPERTY, RULE_TYPE_PROPERTY, parent_style_sheet, parent_rule);
    if (JS_IsException(obj)) { css_property_names_free(&names); return obj; }
    r = JS_GetOpaque(obj, g_rule_class);
    rule_set(ctx, r, &r->property_names, property_names_array(ctx, &names));
    css_property_names_free(&names);
    bl = strlen(block_text);

    /* §3.1 "The syntax Descriptor". Two things can leave the initial `"*"` standing and they are two different
       sentences of the same section: a body that declares no `syntax` at all, and a `syntax` whose value is not
       a single `<string>` or whose string is not a syntax string ("if it returns failure when consume a syntax
       definition is called on it, the descriptor is invalid and must be ignored"). The stored value is the
       string EXACTLY AS SPECIFIED — §6.1's own word — so ` <color># ` keeps its spaces, and §5.4.2's step 1 is
       what strips them for the validity question alone. */
    declared = cssom_declared_value(block_text, bl, "syntax");
    if (declared) {
        syntax = css_property_descriptor_syntax(declared, strlen(declared));
        free(declared);
    }
    if (syntax && !css_property_syntax_definition(syntax, strlen(syntax), &def)) {
        free(syntax);
        syntax = NULL;
    }
    if (!syntax) {
        /* The descriptor is absent or ignored, so §3.1's `Initial: "*"` stands — and it stands as a syntax
           STRING, which is what makes it a definition: it goes through §5.4.2 like any other rather than being
           hand-built here, so `"*"` is §5.4.1's universal syntax definition for the same reason it is one when
           a rule writes it out. */
        star = css_property_syntax_definition("*", 1, &def);
        DCHECK(star && def.universal,
               "§5.4.2 refused §3.1's own initial value for the syntax descriptor — `\"*\"` is step 3's lone "
               "asterisk and IS §5.4.1's universal syntax definition, so this is that step no longer being "
               "reachable rather than a rule that declared something wrong");
        (void)star;
    }
    DCHECK(def.universal || syntax != NULL,
           "an `@property` rule holds a NON-universal syntax definition with no syntax string behind it — the "
           "only definition this rule can have without one is §3.1's initial `\"*\"`, which IS §5.4.1's "
           "universal syntax definition");
    rule_set(ctx, r, &r->property_syntax, JS_NewString(ctx, syntax ? syntax : "*"));
    free(syntax);

    /* §3.2 "The inherits Descriptor" — `Value: true | false`, `Initial: true`. A value that is neither leaves
       the initial standing, by §3's ignore rule. */
    declared = cssom_declared_value(block_text, bl, "inherits");
    if (declared) {
        css_property_descriptor_inherits(declared, strlen(declared), &inherits);
        free(declared);
    }
    rule_set(ctx, r, &r->property_inherits, JS_NewBool(ctx, inherits));

    /* §3.3 "The initial-value Descriptor" — `Value: <declaration-value>?`, `Initial: the guaranteed-invalid
       value`, which §6.1 answers as its nullable `initialValue`'s NULL.
       ITS CROSS-DESCRIPTOR CONDITION IS A VALUE PARSE AGAINST THE OTHER DESCRIPTOR: "If specified, the value of
       the initial-value descriptor must successfully parse according to the rule's syntax descriptor, or else
       the descriptor is invalid and ignored." §4.1 spells out what "according to" means, and it is TWO
       different parses — "parse initialValue according to <declaration-value>? if syntax definition is the
       universal syntax definition, and according to syntax definition otherwise".
       THE UNIVERSAL ARM IS DECIDED BY CONSTRUCTION AND IS NOT A SHORTCUT: lexbor parsed this body as
       declarations, so a value that reached this line already IS a `<declaration-value>` — the production it
       would be re-parsed against is the one it came out of. §3.1's initial `"*"` IS the universal definition,
       so every rule that declares no syntax at all takes this arm too, which is why `initial-value` alone on a
       rule keeps its value.
       THE OTHER ARM IS core/css/css_syntax_match.h, over the components §5.4.3 produced. A value that does not
       match leaves §3.3's initial standing — the guaranteed-invalid value, which §6.1 reports as null — and
       does NOT invalidate the rule, because §3 says an invalid descriptor is "invalid and ignored" and only
       the prelude can drop an `@property`. */
    declared = cssom_declared_value(block_text, bl, "initial-value");
    rule_set(ctx, r, &r->property_initial_value, JS_NULL);
    if (declared) {
        if (def.universal || css_property_syntax_matches(&def, declared, strlen(declared)))
            rule_set(ctx, r, &r->property_initial_value, JS_NewString(ctx, declared));
        free(declared);
    }
    css_syntax_definition_free(&def);
    return obj;
}

static void rule_orphan(JSContext *ctx, JSValueConst rule)
{
    /* THE BRAND IS ASSERTED, NOT THROWN: this is an algorithm §6.4 invokes on a rule it already holds, never a
       member a page can apply to a stranger, and a TypeError here would leave a pending exception in a C
       caller with no member to return it from. */
    CssRuleData *r = rule_of(rule);

    DCHECK(r != NULL, "§6.4's remove a CSS rule was invoked on something that is not a CSS rule");
    rule_set(ctx, r, &r->parent_style_sheet, JS_NULL);
    rule_set(ctx, r, &r->parent_rule, JS_NULL);
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

/* THE CRASH FOR A ROW OF THAT TABLE THAT POINTS AT NOTHING. */
static void rule_alias_unbuilt_fail(const char *written, const char *target)
{
    DFAILF("CSS Compatibility Standard §3.1 \"CSS At-rules\" aliases `@%s` onto `@%s`, and this builder has no "
           "arm for `@%s`. §3.1's table maps a prefixed spelling onto an at-rule the platform ALREADY HAS, so "
           "the thing to build is the UNPREFIXED rule's §6.4 interface, in rule_from_parse, exactly as if the "
           "page had written it unprefixed — and then this row starts working with no further change. Do NOT "
           "answer this by deleting the row: §3.1 says the prefixed spelling MUST be supported, so a missing "
           "row is a page's rule silently vanishing out of `cssRules` rather than a crash anyone will see",
           written, target, target);
}

/* THE AT-KEYWORDS A CSS SPECIFICATION DEFINES — the registry CSS Syntax Level 3 §8 "CSS stylesheets"'s "not
 * recognized" is asked against, and the ONE place that question is answered.
 *
 * IT IS TRANSCRIBED FROM THE CSS WORKING GROUP's OWN CROSS-SPEC AT-RULES INDEX (drafts.csswg.org/indexes/,
 * section "At-rules"), which is where every CSSWG specification's at-rule definitions are collected. That is
 * what makes the table a READING rather than a recollection, and it is what makes each row CHECKABLE: the
 * index links every name to `<spec>/#at-ruledef-<name>`, so the shortname in the second column plus the name in
 * the first reconstructs the exact URL the definition lives at, and one fetch settles any row. The spec
 * SHORTNAME is carried and a section NUMBER deliberately is not — the index publishes the first and not the
 * second, so a number here would be a number nobody read, which §Browser half rates as WORSE than none because
 * it reads as authoritative and sends the next reader to the wrong clause. A number belongs on this row only
 * when it arrives with the interface, which is where `rule_unbuilt_fail`'s list carries them.
 *
 * A ROW IS NOT A CLAIM THAT THIS BUILD IMPLEMENTS THE RULE, and that split is the whole reason the table
 * exists. In the registry with no arm in `rule_from_parse` = a capability this build is missing, and it crashes
 * by name so the next reader builds it. NOT in the registry = no user agent has anything to build, ever, and
 * CSS Syntax §8 says DISCARD. `@counter-style` and `@medium` were one set before this table, and they are
 * opposite facts.
 *
 * `@-webkit-keyframes` IS IN THE INDEX AND IS DELIBERATELY NOT A ROW HERE: CSS Compatibility §3.1 aliases it
 * onto `keyframes` in front of the dispatch, so the resolved name is what reaches this table. `at_rule_dropped`
 * asserts that from the other side, which is what keeps the omission an invariant instead of a hole. */
static bool at_rule_defined(const char *name)
{
    /* SORTED BY NAME (ASCII), which the lookup below depends on and the DCHECK re-establishes on every call. */
    static const struct { const char *name; const char *spec; } AT_RULES[] = {
        { "annotation",          "css-fonts-4" },
        { "apply",               "css-mixins-1" },
        { "bottom-center",       "css-page-3" },
        { "bottom-left",         "css-page-3" },
        { "bottom-left-corner",  "css-page-3" },
        { "bottom-right",        "css-page-3" },
        { "bottom-right-corner", "css-page-3" },
        { "character-variant",   "css-fonts-4" },
        { "charset",             "css-syntax-3" },
        { "color-profile",       "css-color-5" },
        { "container",           "css-conditional-5" },
        { "contents",            "css-mixins-1" },
        { "counter-style",       "css-counter-styles-3" },
        { "custom-media",        "mediaqueries-5" },
        { "custom-selector",     "css-extensions-1" },
        { "else",                "css-conditional-5" },
        { "font-face",           "css-fonts-5" },
        { "font-feature-values", "css-fonts-4" },
        { "font-palette-values", "css-fonts-4" },
        { "function",            "css-mixins-1" },
        { "historical-forms",    "css-fonts-4" },
        { "import",              "css-cascade-6" },
        { "keyframes",           "css-animations-1" },
        { "layer",               "css-cascade-5" },
        { "left-bottom",         "css-page-3" },
        { "left-middle",         "css-page-3" },
        { "left-top",            "css-page-3" },
        { "location",            "css-navigation-1" },
        { "media",               "css-conditional-3" },
        { "mixin",               "css-mixins-1" },
        { "namespace",           "css-namespaces-3" },
        { "navigation",          "css-navigation-1" },
        { "ornaments",           "css-fonts-4" },
        { "page",                "css-page-3" },
        { "position-try",        "css-anchor-position-1" },
        { "private",             "css-mixins-1" },
        { "property",            "css-properties-values-api-1" },
        { "right-bottom",        "css-page-3" },
        { "right-middle",        "css-page-3" },
        { "right-top",           "css-page-3" },
        { "scope",               "css-cascade-6" },
        { "starting-style",      "css-transitions-2" },
        { "styleset",            "css-fonts-4" },
        { "stylistic",           "css-fonts-4" },
        { "supports",            "css-conditional-3" },
        { "supports-condition",  "css-conditional-5" },
        { "swash",               "css-fonts-4" },
        { "top-center",          "css-page-3" },
        { "top-left",            "css-page-3" },
        { "top-left-corner",     "css-page-3" },
        { "top-right",           "css-page-3" },
        { "top-right-corner",    "css-page-3" },
        { "view-transition",     "css-view-transitions-2" },
        { "when",                "css-conditional-5" },
    };
    const unsigned n = (unsigned)(sizeof AT_RULES / sizeof AT_RULES[0]);
    unsigned i;
    bool found = false;

    DCHECK(name != NULL, "CSS Syntax §8's recognized-at-rule registry was asked about no at-keyword");
    for (i = 0; i < n; i++) {
        DCHECK(i == 0 || strcmp(AT_RULES[i - 1].name, AT_RULES[i].name) < 0,
               "the CSS at-rule registry is not sorted by name, or holds one at-keyword twice. It is "
               "transcribed from the CSS Working Group's cross-spec at-rules index and a row inserted out of "
               "order is a row inserted without reading its neighbours — which is exactly how a duplicate or a "
               "misspelling gets in, and a misspelled row silently DISCARDS every rule the standard requires "
               "to be supported under the name it should have carried");
        DCHECK(AT_RULES[i].name[0] != '-' && AT_RULES[i].name[0] != '_',
               "the CSS at-rule registry holds a VENDOR-PREFIXED at-keyword. The only prefixed spelling any "
               "standard defines is CSS Compatibility §3.1's `-webkit-keyframes`, and that is resolved to its "
               "unprefixed name in front of the dispatch — so a prefixed row here is a second answer to a "
               "question §3.1's alias table already answers, able to disagree with it about the interface, the "
               "prototype and the `type`");
        if (strcmp(AT_RULES[i].name, name) == 0) found = true;
    }
    return found;
}

/* THE AT-RULES THAT ARE DROPPED RATHER THAN CRASHED ON, AND BOTH ARMS ARE POSITIVE STATEMENTS ABOUT THE
 * PLATFORM rather than gaps. `name` is the at-keyword as the builder resolved it — §3.1's aliases have already
 * been taken out of it, which is why this function never has to ask about one.
 *
 * `@charset`: CSSOM keeps the historical constant `CHARSET_RULE = 2` and declares NO CSSCharsetRule interface
 * at all, so there is no object an `@charset` could become and every user agent leaves it out of `cssRules`.
 * IT IS ASKED FIRST AND THE ORDER IS LOAD-BEARING: CSS Syntax Level 3 DOES define `@charset`, so it is a row of
 * the registry below, and an arm that ran after it would report a rule the standard defines and this build has
 * no interface for — which is true and is the wrong answer, because there is no interface to build.
 *
 * AN AT-KEYWORD NO CSS SPECIFICATION DEFINES: CSS Syntax Level 3 §8 "CSS stylesheets" states the outcome in
 * one sentence — "If any style rule is invalid, or any at-rule is NOT RECOGNIZED or is invalid according to its
 * grammar or context, it's a parse error. DISCARD THAT RULE." CSS 2.1 §4.2 "Rules for handling parsing errors"
 * says the same thing under the heading "At-rules with unknown at-keywords" ("User agents must ignore an
 * invalid at-keyword together with everything following it, up to the end of the block that contains the
 * invalid at-keyword") — and its WORKED EXAMPLE is `@three-dee`, an at-keyword with no dash and no underscore
 * which "is not part of CSS 2.1. Therefore, the whole at-rule … is ignored." A vendor-prefixed at-keyword is
 * one member of that set and not a category of its own: `@-moz-keyframes`, `@-ms-viewport` and
 * `@-moz-document` are dropped because no specification defines them for THIS user agent, which is the same
 * reason `@three-dee` is. (`@-webkit-keyframes` is the one exception and it never reaches here: CSS
 * Compatibility §3.1 aliases it onto `keyframes` in front of the dispatch, which the DCHECK below asserts.)
 *
 * THIS REPLACES A TEST ON THE SHAPE OF THE NAME, WHICH WAS SPEC-WRONG AND KILLED WHOLE DOCUMENTS. The deleted
 * predicate dropped an at-keyword beginning with `-` or `_` and crashed on every other unknown one, inferring
 * from CSS 2.1 §4.2's other half ("CSS 2.1 reserves for future updates of CSS all … @-keywords that do not
 * contain an identifier beginning with dash or underscore") that an unprefixed unknown at-keyword must be a
 * specification's interface missing here. That inference reads a sentence about WHO MAY DEFINE the keyword as
 * a sentence about WHAT A UA DOES when it meets one, and §4.2's own `@three-dee` example refutes it directly.
 * The cost was not theoretical: CSS-in-JS runtimes emit breakpoint at-keywords verbatim when a theme token
 * fails to resolve, so a live site's inline `<style>` carrying `@medium{…}` beside its `@media` rules aborted
 * the instance at sheet-build time, with ZERO flows run and therefore zero endpoints and zero sinks — the
 * whole document lost to a rule real Chrome discards without comment.
 *
 * AND IT IS STILL NOT AN UNKNOWN-AT-RULE FALLBACK. The set is closed by a REGISTRY of the at-keywords CSS
 * specifications define (`at_rule_defined`), not by a fallback: a name IN that registry with no arm in
 * `rule_from_parse` is a capability this build is missing and goes on crashing by name below, which is the
 * signal that crash exists to deliver. What changed is only that a name NO standard defines is no longer
 * mistaken for one — the two were one set wearing one predicate, and separating them is what lets each keep
 * its own answer. */
static bool at_rule_dropped(const char *name)
{
    if (strcmp(name, "charset") == 0) return true;
    DCHECK(at_rule_alias(name) == NULL,
           "a `-webkit-` at-keyword CSS Compatibility §3.1 aliases onto a real at-rule reached the DROP "
           "predicate. The builder resolves §3.1's table before it dispatches, so an aliased name must have "
           "become the unprefixed one long before this line — reaching it means the resolution was moved "
           "after the dispatch, which silently deletes every rule the standard requires to be supported");
    return !at_rule_defined(name);
}

/* The TYPE of the rule this one is written inside, or 0 at a sheet's top level. What a rule may BE depends on
   it — CSS Syntax's rules are context-sensitive and CSS Paged Media §4.3 states two of them outright. */
static uint16_t enclosing_rule_type(JSValueConst parent_rule)
{
    CssRuleData *r;

    if (JS_IsNull(parent_rule)) return 0;
    r = rule_of(parent_rule);
    DCHECK(r != NULL, "a rule's enclosing rule is something that is not a CSS rule");
    return r ? r->type : (uint16_t)0;
}

/* THE NEAREST ANCESTOR STYLE RULE of a rule written inside `parent_rule`, or JS_NULL when there is none — which
   is CSS Nesting §4 "Nesting Selector: the & selector"'s "the parent rule" and therefore the one thing that
   decides whether a qualified rule here is a NESTED style rule at all. BORROWED: the record owns it.
   IT IS A WALK AND NOT A LOOK AT THE IMMEDIATE PARENT, and §3.3 "Nesting Other At-Rules" is why: a nested group
   rule's block is parsed as `<block-contents>`, in which "Style rules are nested style rules, with their
   nesting selector taking its definition from the NEAREST ANCESTOR STYLE RULE". So in
   `.a { @media print { .b { } } }` the rule `.b` is nested and its `&` is `.a`, two levels up, with an
   `@media` in between that is not a style rule and has no selector of its own to be relative to.
   THERE IS NO DEPTH BOUND AND THERE MAY NOT BE ONE: §3.3 nests group rules inside style rules inside group
   rules without limit, and the walk is over the chain the parse already built. */
static JSValueConst rule_nesting_parent(JSValueConst parent_rule)
{
    JSValueConst cur = parent_rule;

    while (!JS_IsNull(cur)) {
        CssRuleData *r = rule_of(cur);

        DCHECK(r != NULL, "a rule's enclosing chain holds something that is not a CSS rule");
        if (!r) return JS_NULL;
        if (r->type == RULE_TYPE_STYLE) return cur;
        cur = r->parent_rule;
    }
    return JS_NULL;
}

static JSValue rule_from_parse(RuleBuild *b, const CssomRule *pr, JSValueConst parent_rule)
{
    uint16_t enclosing = enclosing_rule_type(parent_rule);
    const char *at;

    /* CSS Paged Media §4.3: "The @page rule can only contain page properties and margin at-rules." So inside
       an `@page` the ONLY rules are §4.3's sixteen margin at-rules, and everything else written there — a
       style rule, a nested `@page`, an `@media` — is invalid IN THIS CONTEXT and CSS Syntax drops it. That is
       not a gap this build has an interface for: `@page :first { h1 { color: #444 } }` is in
       css/cssom/cssom-ruleTypeAndOrder.html precisely because a page rule has no style rule in it anywhere. */
    if (enclosing == RULE_TYPE_PAGE) {
        if (!pr->at_name || !css_page_margin_at_rule(pr->at_name) || !pr->has_block) return JS_UNDEFINED;
        return margin_rule_new(b->ctx, b->sheet, parent_rule, pr->at_name, pr->block ? pr->block : "");
    }
    /* CSS Animations §3: "The <rule-list> inside of @keyframes can only contain <keyframe-block> rules." So
       inside a `@keyframes` the ONLY rule is a qualified rule whose prelude is a `<keyframe-selector>#`, and
       everything else written there — an at-rule of any kind, a qualified rule whose prelude is neither a
       keyframe selector list nor anything else — is invalid IN THIS CONTEXT and CSS Syntax drops it.
       BOTH SHAPES OF QUALIFIED RULE ARRIVE HERE, and that is not an accident of the parser: `from { }` and
       `to { }` are valid SELECTOR lists (they are type selectors), so lexbor parses them as style rules and
       `pr->prelude` is the serialized selector; `0%, 100% { }` is not a selector list at all, so it arrives
       with the raw prelude and `prelude_is_selectors` unset. Either way the text is what §3's grammar reads,
       and it decides which of them is a keyframe block. */
    if (enclosing == RULE_TYPE_KEYFRAMES) {
        if (pr->at_name) return JS_UNDEFINED;
        return keyframe_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude, pr->block ? pr->block : "");
    }
    /* AND THE MIRROR: a margin at-rule is only a rule INSIDE an `@page`. Anywhere else `@top-left { }` is an
       at-rule no specification defines, which CSS Syntax drops — so it is dropped here rather than reaching
       the crash below, which would name a capability that is already built. */
    if (pr->at_name && css_page_margin_at_rule(pr->at_name)) return JS_UNDEFINED;
    /* A QUALIFIED RULE INSIDE A STYLE RULE IS CSS NESTING §3's NESTED STYLE RULE, and it differs from the one
       below in exactly the way §3.1 "Syntax" says it does: "A nested style rule accepts a
       <relative-selector-list> as its prelude (rather than just a <selector-list>)". This engine's selector
       parser implements neither the nesting selector nor a relative selector list, so BOTH extra shapes reach
       here as a prelude it refused — `&:hover` and `> .baz` arrive exactly as `!!!` does — and telling them
       apart is what §3.1's shape test is for. Getting it wrong in the other direction is what a
       drop-everything-refused rule DID: every `&`-written nested rule vanished from `cssRules` and out of the
       cascade with it, which is the silent version of the page's styles simply being wrong.
       WHAT IS STORED IS §6 "CSSOM"'s ABSOLUTIZED FORM — "When serializing a relative selector in a nested style
       rule, the selector must be absolutized, with the implied nesting selector inserted" — so `.bar` nested
       inside `.foo` has a `selectorText` of `& .bar`, and every later reader (the cascade above all) has ONE
       shape to resolve instead of three. Its VALIDITY is not decided here: §3.1's "An invalid nested style rule
       is ignored, along with its contents" is discharged by the cascade, which parses the RESOLVED text and
       emits nothing for a rule that is not a selector list — the same parse that decides validity for every
       other selector in the sheet. */
    if (!pr->at_name && !JS_IsNull(rule_nesting_parent(parent_rule))) {
        size_t plen = strlen(pr->prelude);
        char *absolutized;
        JSValue out;

        if (!pr->prelude_is_selectors && !css_nesting_is_relative(pr->prelude, plen)) return JS_UNDEFINED;
        absolutized = css_nesting_absolutize(pr->prelude, plen);
        out = style_rule_new(b->ctx, b->sheet, parent_rule, absolutized, pr->block ? pr->block : "");
        free(absolutized);
        return out;
    }
    /* AND ITS MIRROR: outside a `@keyframes` a qualified rule whose prelude is not a selector list is an
       invalid STYLE rule, which CSS Syntax drops. `0%, 100% { }` at a sheet's top level is that, and so is
       `!!! { }` — the context is what makes the first one a rule elsewhere and nothing makes the second one
       one anywhere. */
    if (!pr->at_name && !pr->prelude_is_selectors) return JS_UNDEFINED;
    if (!pr->at_name)
        return style_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude, pr->block ? pr->block : "");
    /* CSS COMPATIBILITY STANDARD §3.1 "CSS At-rules", RESOLVED ONCE, IN FRONT OF THE DISPATCH. `at` is the
       at-keyword the arms below decide from and `pr->at_name` stays the spelling the page WROTE; a creator
       that has to emit the written one takes it as an argument. Resolving here rather than in an arm is what
       makes an alias a SPELLING: every question after this line — the grammar of the prelude, which interface,
       which prototype, which `type`, what the body may contain — is answered by the one arm the unprefixed
       name reaches, so the two spellings cannot drift apart in any of them. Adding §3.1's next row is then a
       table entry and nothing else. */
    at = at_rule_alias(pr->at_name);
    if (!at) at = pr->at_name;
    if (strcmp(at, "media") == 0) {
        DCHECK(pr->has_block,
               "an `@media` rule reached the builder with no block. CSS Syntax makes a block at-rule without "
               "one INVALID, and cssom_parse_rules drops an invalid at-rule before it is ever reported, so a "
               "block-less `@media` here means the parse kept a rule it should have discarded");
        return media_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude);
    }
    /* CSS Conditional §6 makes `@supports` a BLOCK at-rule (`@supports <supports-condition> { <rule-list> }`),
       so `@supports (display:flex);` is an at-rule whose grammar failed and CSS Syntax drops it — the same
       shape `@font-face;` and `@page;` have, and dropped here for the same reason: lexbor parses an at-rule it
       does not know as `_CUSTOM`, which accepts both, so this is malformed author CSS and not an engine
       invariant. The CONDITION can drop it too, and that is the second half of the same sentence — see
       supports_rule_new, which is where §6's "processors must ignore such a rule" lives.
       ITS BODY IS A RULE LIST AND NOTHING ELSE (§6's `<rule-list>`), so the declarations the parse reports for
       it are not read — the same sentence `@keyframes` and `@layer` get, and for the same reason: a
       declaration written where §6 admits only rules would be CSSOM's CSSNestedDeclarations, a rule interface
       this build does not have and whose absence the parse walk already records. */
    if (strcmp(at, "supports") == 0)
        return pr->has_block ? supports_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude) : JS_UNDEFINED;
    /* CSS Conditional 5 §5.4 makes `@container` a BLOCK at-rule (`@container <container-condition># {
       <rule-list> }`) exactly as §6 does `@supports`, so `@container card (width > 0px);` is an at-rule whose
       grammar failed and CSS Syntax §8 drops it — the same sentence and the same JS_UNDEFINED as the arm above.
       ITS BODY IS A RULE LIST AND NOTHING ELSE, so the declarations the parse reports for it are not read, for
       the reason `@supports` and `@layer` do not read theirs: a declaration written where §5.4 admits only
       rules would be CSSOM's CSSNestedDeclarations, a rule interface this build does not have and whose absence
       the parse walk already records. */
    if (strcmp(at, "container") == 0)
        return pr->has_block ? container_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude) : JS_UNDEFINED;
    /* CSS Cascade §2 makes `@import` a STATEMENT at-rule terminated by a semicolon, so `@import url(x) {}` is
       an at-rule whose grammar failed and CSS Syntax DROPS it. It is dropped HERE and not asserted against,
       because the shape reaches this file from the PAGE: lexbor parses an at-rule it does not know as
       `_CUSTOM`, which accepts a block, so this is malformed author CSS and not an engine invariant. */
    if (strcmp(at, "import") == 0)
        return pr->has_block ? JS_UNDEFINED : import_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude);
    if (strcmp(at, "namespace") == 0) {
        DCHECK(!pr->has_block,
               "an `@namespace` rule reached the builder WITH a block. Lexbor's own namespace state marks the "
               "parse failed the moment it meets one and converts the rule to `_UNDEF`, which the walk drops, "
               "so a block here means that conversion did not happen");
        return namespace_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude);
    }
    /* And the mirror of it: CSS Fonts §12 makes `@font-face` a BLOCK at-rule, so `@font-face;` is invalid and
       dropped. Lexbor keeps this one rather than converting it (its `font_face_end` stores the returned block
       whether or not there is one, where `media_end` checks), so the drop is this file's. */
    if (strcmp(at, "font-face") == 0) {
        DCHECK(!pr->has_block || pr->block != NULL,
               "an `@font-face` rule reached the builder with a block whose DESCRIPTORS were not reported. "
               "cssom_parse_rules serializes an `@font-face` body as declarations precisely because it is one, "
               "so a null block beside a live one means that arm did not run");
        return pr->has_block ? font_face_rule_new(b->ctx, b->sheet, parent_rule, pr->block ? pr->block : "")
                             : JS_UNDEFINED;
    }
    /* CSS Paged Media §4.3 makes `@page` a BLOCK at-rule (`@page <page-selector-list>? { … }`), so `@page;` is
       an at-rule whose grammar failed and CSS Syntax drops it — the same shape `@font-face;` has, and dropped
       here for the same reason it is: lexbor parses an at-rule it does not know as `_CUSTOM`, which accepts
       both, so this is malformed author CSS and not an engine invariant. */
    if (strcmp(at, "page") == 0)
        return pr->has_block ? page_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude,
                                             pr->block ? pr->block : "")
                             : JS_UNDEFINED;
    /* CSS Animations §3 makes `@keyframes` a BLOCK at-rule (`@keyframes <keyframes-name> {
       <qualified-rule-list> }`), so `@keyframes foo;` is an at-rule whose grammar failed and CSS Syntax drops
       it — the same shape `@font-face;` and `@page;` have, and dropped here for the same reason. Its BODY is
       a rule list and nothing else, so the declarations the parse reports for it are not read: a declaration
       written directly inside a `@keyframes` is not in a `<keyframe-block>` and §3 admits nothing else. */
    if (strcmp(at, "keyframes") == 0)
        return pr->has_block ? keyframes_rule_new(b->ctx, b->sheet, parent_rule, pr->at_name, pr->prelude)
                             : JS_UNDEFINED;
    /* CSS Cascade §6.4.4 gives `@layer` TWO grammars and the BLOCK is what tells them apart: §6.4.4.1's block
       at-rule is `@layer <layer-name>? { <rule-list> }` and §6.4.4.2's statement at-rule is
       `@layer <layer-name>#;`. So `has_block` forks here as it does for `@import` and `@font-face` — and this
       is the one place in this builder where it chooses between two INTERFACES rather than between a rule and a
       drop, because both shapes are real rules.
       ITS BODY IS A RULE LIST AND NOTHING ELSE (§6.4.4.1's `<rule-list>`), so the declarations the parse
       reports for it are not read — the same sentence `@keyframes` gets and for the same reason. A declaration
       written where §6.4.4.1 admits only rules is invalid in that context and CSS Syntax drops it; inside a
       NESTED `@layer` it would be CSSOM's CSSNestedDeclarations, a rule interface this build does not have and
       whose absence the parse walk already records. */
    /* CSS Properties and Values API 1 §3 makes `@property` a BLOCK at-rule (`@property <custom-property-name>#
       { <declaration-list> }`), so `@property --x;` is an at-rule whose grammar failed and CSS Syntax drops it
       — the same shape `@font-face;` and `@page;` have, and dropped here for the same reason.
       ITS BODY IS DECLARATIONS AND NOTHING ELSE, so `pr->block` is read and no child rule of it can be one: a
       rule written inside a `<declaration-list>` is invalid in that context, which is the sentence `@font-face`
       and a `<keyframe-block>` already get and which `rule_built` applies from the other side through
       `rule_type_has_child_rules`. */
    if (strcmp(at, "property") == 0)
        return pr->has_block ? property_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude,
                                                 pr->block ? pr->block : "")
                             : JS_UNDEFINED;
    if (strcmp(at, "layer") == 0)
        return pr->has_block ? layer_block_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude)
                             : layer_statement_rule_new(b->ctx, b->sheet, parent_rule, pr->prelude);
    if (at_rule_dropped(at)) return JS_UNDEFINED;
    /* AN ALIAS WHOSE TARGET NO ARM ABOVE ANSWERS IS THE TABLE DISAGREEING WITH THIS BUILDER, and it is a
       different crash from the one below on purpose: the message below names the at-keyword THE PAGE SHIPPED
       and tells its reader to build that interface, which is the wrong instruction here — the page shipped a
       spelling the standard says is already supported, and what is missing is the rule it was aliased ONTO.
       It cannot be reached by a page: only a row of the table above can put a name here. */
    if (at != pr->at_name) rule_alias_unbuilt_fail(pr->at_name, at);
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
        /* THE ENCLOSING RULE CONTAINS NO RULES AT ALL, so this one is not in it either. An `@font-face`'s, a
           margin at-rule's and a `<keyframe-block>`'s body is CSS Syntax's `<declaration-list>`, which admits
           declarations and nothing else, so a rule written inside one is invalid and dropped — and it must be
           dropped HERE, because `rule_child_rules` asserts its receiver has a child list and none of those
           does. */
        if (!rule_type_has_child_rules(enclosing_rule_type(parent_rule)))
            return build_push(b, JS_UNDEFINED);
        list = rule_child_rules(b->ctx, parent_rule);
    }
    rule = rule_from_parse(b, pr, parent_rule);
    if (JS_IsException(rule) || JS_IsUndefined(rule)) {
        JS_FreeValue(b->ctx, list);
        JS_FreeValue(b->ctx, rule);
        return build_push(b, JS_UNDEFINED);
    }
    JS_SetPropertyUint32(b->ctx, list, array_len(b->ctx, list), JS_DupValue(b->ctx, rule));
    JS_FreeValue(b->ctx, list);
    if (!parent) b->n_top++;
    return build_push(b, rule);
}

/* WHAT §6.4 DECLARES FOR AN AT-RULE THIS BUILD HAS NO ARM FOR — one row per at-keyword in `at_rule_defined`'s
 * registry that `rule_from_parse` does not mint, and the reason the crash below is a LOOKUP rather than a
 * paragraph.
 *
 * THE PARAGRAPH WAS A CLAIM AND IT HAD ALREADY BEEN WRONG IN THE DIRECTION THAT COSTS MOST — TWICE. It once
 * named four interfaces and omitted the one that fires: a single `@property --x { … }` in a shipping site's
 * stylesheet aborted that instance at stage `create` with ZERO flows run, and the reader standing at that abort
 * was told to build one of four things, none of them the one in front of them. It was then rewritten to name
 * three that remain — CSSScopeRule, CSSCounterStyleRule, CSSFontFeatureValuesRule — while the registry beside
 * it recognises TWENTY-EIGHT at-keywords this builder has no arm for. Every one of the other twenty-five was a
 * page whose `@WHY` would have said "what remains is" and then listed three things that were not it. That is
 * the stale-`DFAIL` failure mode with a spec behind it: authoritative, wrong, and followed.
 *
 * A PROSE LIST CANNOT BE CHECKED AGAINST THE REGISTRY AND A TABLE CAN, WHICH IS THE WHOLE CHANGE. The two
 * assertions below are what make this a mechanism instead of a better-maintained sentence: every row here names
 * an at-keyword `at_rule_defined` recognises (so a row cannot be invented), and every at-keyword that REACHES
 * the crash has a row (so the list cannot silently under-report — the miss fires at the exact page that
 * exposed it). A name comes off this table in the same diff that builds its interface, and the second assert
 * is what makes forgetting that impossible rather than merely discouraged.
 *
 * THE THIRD STATE IS THE ONE A LIST OF INTERFACES CANNOT EXPRESS, AND IT IS NOT RARE. `@when`, `@else`,
 * `@private`, `@navigation`, `@location` and `@custom-selector` are at-rules a CSS specification DEFINES — so
 * CSS Syntax §8 forbids discarding them and `at_rule_defined` correctly answers yes — for which NO
 * specification declares a CSSOM interface at all. CSS Extensions 1 §3.2 "CSSOM" is, in its entirety, the words
 * "Fill in." Telling that reader to "build the interface" sends them looking for one that does not exist, which
 * is the same wrong-and-authoritative failure one level down; so those rows carry a NULL interface and the
 * crash says what is actually true — there is nothing to build until the standard says what.
 *
 * EVERY SECTION NUMBER HERE CARRIES ITS SECTION TITLE AND WAS READ OFF THE SPECIFICATION, never recalled.
 * Numbers renumber and titles survive, so a row whose two halves stop agreeing is visible instead of silent.
 * The registry deliberately carries a spec SHORTNAME and no number, because the CSSWG index it is transcribed
 * from publishes the first and not the second; a number belongs to the row that arrives WITH the interface,
 * which is this one. */
static const struct {
    const char *at;          /* the at-keyword, spelled as `at_rule_defined`'s registry spells it */
    const char *interface;   /* the CSSOM interface a standard declares for it, or NULL when none does */
    const char *where;       /* that declaration's spec, section NUMBER and section TITLE */
} RULE_UNBUILT[] = {
    /* SORTED BY at-keyword (ASCII), which the DCHECK below re-establishes on every crash. */
    { "annotation",         "CSSFontFeatureValuesRule",
      "CSS Fonts 4 §12.2 \"The CSSFontFeatureValuesRule interface\" — `@annotation` is NOT a rule of its own: "
      "it is one of that interface's seven CSSFontFeatureValuesMap attributes, so building §12.2 builds this" },
    { "apply",              "CSSApplyBlockRule / CSSApplyStatementRule",
      "CSS Mixins 1 §7.4 \"The CSSApplyBlockRule Interface\" and §7.5 \"The CSSApplyStatementRule Interface\" — "
      "TWO interfaces, chosen by whether the `@apply` the page wrote has a block" },
    { "character-variant",  "CSSFontFeatureValuesRule",
      "CSS Fonts 4 §12.2 \"The CSSFontFeatureValuesRule interface\" — its `characterVariant` map attribute" },
    { "color-profile",      "CSSColorProfileRule",
      "CSS Color 5 §12.1 \"The CSSColorProfileRule interface\"" },
    { "contents",           "CSSContentsBlockRule / CSSContentsStatementRule",
      "CSS Mixins 1 §7.6 \"The CSSContentsBlockRule Interface\" and §7.7 \"The CSSContentsStatementRule "
      "Interface\" — two, split by the block exactly as `@apply`'s pair is" },
    { "counter-style",      "CSSCounterStyleRule",
      "CSS Counter Styles 3 §9.2 \"The CSSCounterStyleRule interface\" — and it is one of only TWO rows here "
      "whose §6.4.2 TYPE NUMBER is already declared (COUNTER_STYLE_RULE = 11) with no interface behind it" },
    { "custom-media",       "CSSCustomMediaRule",
      "Media Queries 5 §11 \"CSSOM\"" },
    { "custom-selector",    NULL,
      "CSS Extensions 1 §3 \"Custom Selectors\" defines `@custom-selector`, and its §3.2 \"CSSOM\" is the two "
      "words \"Fill in.\" — there is no interface to build and none to wait for but the standard's" },
    { "else",               NULL,
      "CSS Conditional 5 §4 \"Chained Conditionals: the @else rule\" defines the rule; that specification's §9 "
      "\"APIs\" declares CSSContainerRule and CSSSupportsConditionRule and NOTHING for `@else`" },
    { "font-feature-values","CSSFontFeatureValuesRule",
      "CSS Fonts 4 §12.2 \"The CSSFontFeatureValuesRule interface\" — the other row whose §6.4.2 type number is "
      "declared ahead of it (FONT_FEATURE_VALUES_RULE = 14). Its seven inner at-rules are its map attributes, "
      "so this one interface answers all eight registry rows" },
    { "font-palette-values","CSSFontPaletteValuesRule",
      "CSS Fonts 4 §12.3 \"The CSSFontPaletteValuesRule interface\"" },
    { "function",           "CSSFunctionRule",
      "CSS Mixins 1 §7.1 \"The CSSFunctionRule Interface\" — and its body's declarations are §7.2 \"The "
      "CSSFunctionDeclarations Interface\", which is a second object this one has to mint" },
    { "historical-forms",   "CSSFontFeatureValuesRule",
      "CSS Fonts 4 §12.2 \"The CSSFontFeatureValuesRule interface\" — its `historicalForms` map attribute" },
    { "location",           NULL,
      "CSS Navigation 1 §1.2 \"Declaring named URL patterns: the @location rule\" defines the rule; that "
      "specification declares no IDL whatever" },
    { "mixin",              "CSSMixinRule",
      "CSS Mixins 1 §7.3 \"The CSSMixinRule Interface\"" },
    { "navigation",         NULL,
      "CSS Navigation 1 §3.1 \"Navigation queries: the @navigation rule\" defines the rule; that specification "
      "declares no IDL whatever" },
    { "ornaments",          "CSSFontFeatureValuesRule",
      "CSS Fonts 4 §12.2 \"The CSSFontFeatureValuesRule interface\" — its `ornaments` map attribute" },
    { "position-try",       "CSSPositionTryRule",
      "CSS Anchor Positioning 1 §8.1 \"The CSSPositionTryRule interface\" — whose descriptors are that same "
      "section's CSSPositionTryDescriptors" },
    { "private",            NULL,
      "CSS Mixins 1 §6 \"Private Custom Properties: the @private rule\" defines the rule; that specification's "
      "§7 \"CSSOM\" declares seven interfaces and none of them is for `@private`" },
    { "scope",              "CSSScopeRule",
      "CSS Cascade 6 §4.1 \"The CSSScopeRule interface\" — no §6.4.2 type number at all (that table is frozen, "
      "so its `type` is 0, like the CSSLayer*, CSSProperty and CSSContainer rules already built)" },
    { "starting-style",     "CSSStartingStyleRule",
      "CSS Transitions 2 §3.3.1 \"The CSSStartingStyleRule interface\"" },
    { "styleset",           "CSSFontFeatureValuesRule",
      "CSS Fonts 4 §12.2 \"The CSSFontFeatureValuesRule interface\" — its `styleset` map attribute" },
    { "stylistic",          "CSSFontFeatureValuesRule",
      "CSS Fonts 4 §12.2 \"The CSSFontFeatureValuesRule interface\" — its `stylistic` map attribute" },
    { "supports-condition", "CSSSupportsConditionRule",
      "CSS Conditional 5 §9.2 \"The CSSSupportsConditionRule interface\"" },
    { "swash",              "CSSFontFeatureValuesRule",
      "CSS Fonts 4 §12.2 \"The CSSFontFeatureValuesRule interface\" — its `swash` map attribute" },
    { "view-transition",    "CSSViewTransitionRule",
      "CSS View Transitions 2 §8.3.3 \"Accessing the @view-transition rule using CSSOM\"" },
    { "when",               NULL,
      "CSS Conditional 5 §3 \"Generalized Conditional Rules: the @when rule\" defines the rule; that "
      "specification's §9 \"APIs\" declares CSSContainerRule and CSSSupportsConditionRule and nothing for it" },
};

/* THE CRASH THAT NAMES WHAT TO BUILD. It is a function so that the at-rule's own name is in the message: the
   reader of a `@WHY` is standing at the rule the page shipped, and "an at-rule" tells them nothing about which
   interface to write. It now names THAT rule's interface and the clause it is declared in, rather than reciting
   a list the reader has to find their own rule in — which is what let the list be wrong twice without anybody
   noticing, because a reader who cannot find their rule reads the omission as "not covered yet" either way.
   TRUNCATION IS ASSERTED RATHER THAN ARITHMETIC. The buffer used to be sized as `sizeof` the format literal
   plus the name bound, which was right until a substitution stopped being the name: these rows are prose and
   `sizeof` a `%s` is two. A fixed 600 had already truncated the 839-byte text once, silently deleting the one
   sentence the crash exists to deliver, at the one moment somebody was reading it. `snprintf` returns the
   length it WANTED, so the check is exact and cannot go stale when the next row is longer than this one. */
static void rule_unbuilt_fail(const char *name)
{
/* TWO SHAPES, ONE FORMAT, AND THE CITATION IS THE SAME FIELD IN BOTH — which is what stops the no-interface
   arm from being the arm that quietly loses its section number. The four substitutions after the name are the
   lead-in, the interface (empty in the second shape), the `where` citation both shapes end on, and the ACTION.
   THE ACTION IS PER SHAPE AND THAT IS THE POINT OF SPLITTING IT OUT. A single trailing "mint it in
   rule_from_parse" told the reader of a `@when` — for which no standard declares an interface — to go and
   build one, which is the wrong-and-authoritative failure this whole table exists to end, reproduced at the
   last sentence of the very crash that ends it. */
#define RULE_UNBUILT_FMT                                                                                       \
    "CSSOM §6.4 has no interface built for the at-rule `@%s`, so a stylesheet containing one cannot be "        \
    "represented. §6.4.4's CSSImportRule, §6.4.5's CSSGroupingRule, §6.4.7's CSSPageRule, §6.4.8's "            \
    "CSSMarginRule and §6.4.9's CSSNamespaceRule, CSS Conditional 3 §7.2's CSSConditionRule, §7.3's "           \
    "CSSMediaRule and §7.4's CSSSupportsRule, CSS Conditional 5 §9.1's CSSContainerRule, CSS Fonts 5 §9.1's "   \
    "CSSFontFaceRule, CSS Animations 1 §6.2's CSSKeyframeRule and §6.3's CSSKeyframesRule, CSS Cascade 5 "      \
    "§8.1's CSSLayerBlockRule and §8.2's CSSLayerStatementRule, and CSS Properties and Values API 1 §6.1's "    \
    "CSSPropertyRule are built. %s%s — %s. %s"

    static const char ACT_BUILD[] =
        "Mint it in rule_from_parse and strike its row off RULE_UNBUILT in the SAME diff — the second of this "
        "function's two asserts is what makes forgetting that half impossible. Do NOT skip the rule, because "
        "every index after it would then name a different rule than the page's";
    static const char ACT_NONE[] =
        "This is a STANDARDS gap and not a gap here, so there is no arm to write in rule_from_parse: the rule "
        "is recognised (CSS Syntax §8 forbids discarding it) and has no object it can become. Do NOT invent an "
        "interface, and do NOT skip the rule — skipping renumbers every rule after it. What this needs is the "
        "standard, so take it there";
    const unsigned n = (unsigned)(sizeof RULE_UNBUILT / sizeof RULE_UNBUILT[0]);
    unsigned i, hit = n;

    for (i = 0; i < n; i++) {
        DCHECK(i == 0 || strcmp(RULE_UNBUILT[i - 1].at, RULE_UNBUILT[i].at) < 0,
               "the §6.4 unbuilt-interface table is not sorted by at-keyword, or holds one twice. A duplicate "
               "row is two answers to one question — able to name two different interfaces for the rule in "
               "front of the reader — and a row inserted out of order is a row inserted without reading its "
               "neighbours, which is how the duplicate gets in");
        DCHECK(at_rule_defined(RULE_UNBUILT[i].at),
               "the §6.4 unbuilt-interface table names an at-keyword `at_rule_defined`'s registry does NOT "
               "recognise. The registry decides what CSS Syntax §8 discards, so a row here for a name it does "
               "not hold describes an interface for a rule that never reaches this crash — the row is a "
               "recollection rather than a reading, which is exactly what this table replaced");
        if (strcmp(RULE_UNBUILT[i].at, name) == 0) hit = i;
    }
    if (hit == n) {
        DFAILF("`@%s` is in CSS Syntax §8's recognized-at-rule registry, has no arm in rule_from_parse, and "
               "has NO ROW in the §6.4 unbuilt-interface table — so this crash cannot say what to build. Add "
               "the row (interface, spec, section NUMBER and section TITLE, read off the specification and "
               "never recalled; a NULL interface where no standard declares one) in the same diff that finds "
               "this. THIS assert is the half that stops the list under-reporting, which it has done twice: "
               "it fires at the exact page that exposed the gap, which is the only moment anybody is looking",
               name);
        return;
    }
    DFAILF(RULE_UNBUILT_FMT, name,
           RULE_UNBUILT[hit].interface ? "The interface to build is "
                                       : "NO specification declares a CSSOM interface for it",
           RULE_UNBUILT[hit].interface ? RULE_UNBUILT[hit].interface : "",
           RULE_UNBUILT[hit].where,
           RULE_UNBUILT[hit].interface ? ACT_BUILD : ACT_NONE);
#undef RULE_UNBUILT_FMT
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

/* THE ZONES A RULE OF THIS TYPE MAY OCCUPY — see the ZONE_ enum for the three sentences the table transcribes. */
static unsigned rule_zones(uint16_t type)
{
    if (type == RULE_TYPE_IMPORT) return ZONE_BIT(ZONE_IMPORT);
    if (type == RULE_TYPE_NAMESPACE) return ZONE_BIT(ZONE_NAMESPACE);
    /* THE ONE TYPE WITH TWO, and the GAP between them is as load-bearing as the bits: §6.4.4.2 allows a `@layer`
       statement before the prologue AND wherever a block at-rule is (which is wherever any rule is), and its
       note forbids exactly what lies between — "no @layer rules are allowed between @import and @namespace
       rules". */
    if (type == RULE_TYPE_LAYER_STATEMENT) return ZONE_BIT(ZONE_LEAD) | ZONE_BIT(ZONE_BODY);
    return ZONE_BIT(ZONE_BODY);
}

/* The EARLIEST zone `zones` admits that is not before `*pfloor`, TAKEN. False when there is none, which is the
   sequence the prologue does not match. Taking the earliest is never wrong and so this needs no search: a
   smaller floor admits every assignment a larger one does, so a greedy walk that fails has no assignment it
   could have missed. */
static bool zone_take(unsigned zones, int *pfloor)
{
    int z;

    DCHECK(zones != 0, "a CSS rule type may occupy NO zone of a style sheet — every rule this build makes is a "
                       "rule some sheet can hold, so an empty set is the zone table having lost a row");
    for (z = *pfloor; z < ZONE_N; z++)
        if (zones & ZONE_BIT(z)) { *pfloor = z; return true; }
    return false;
}

/* §6.4 STEP 5 — "if new rule cannot be inserted into list at the zero-indexed position index due to
   constraints specified by CSS, throw a HierarchyRequestError", whose own note is "for example, a CSS style
   sheet cannot contain an @import at-rule after a style rule".
     - `nested` set (§6.4.5's insertRule, into a grouping rule): an `@import` and an `@namespace` cannot go
       inside one AT ALL — CSS Cascade §2 and CSS Namespaces §2 both state their position relative to a STYLE
       SHEET, and neither is a rule a conditional group may contain.
     - `nested` unset: the sheet's rules must match the PROLOGUE the ZONE_ enum transcribes, so the constraint
       is that the insertion must not break it. Asking it of the RESULTING list is what makes both directions
       one walk: a style rule inserted at index 0 of a sheet holding an `@import` is refused by the same line
       that refuses an `@import` inserted after one, because each leaves the same list.
   CSS Paged Media §4.3's "the @page rule can only contain page properties and margin at-rules" is NOT asked
   here, and that is not an omission: step 3's parse runs with the enclosing rule already in hand (see
   `rule_from_parse`, which is reached from this algorithm and from the sheet parse alike), so a style rule
   written into an `@page` is dropped by CSS Syntax before step 4 counts the rules and the answer is step 4's
   SyntaxError. Asking again here would be an arm no input can reach. */
static bool insert_position_ok(JSContext *ctx, JSValueConst list, uint32_t index, uint16_t type, bool nested)
{
    uint32_t n, k;
    int zone_floor = ZONE_LEAD;

    /* A `@layer` is not among the two, and both halves of §6.4.4 say so: §6.4.4.2 admits the statement
       "everywhere @layer block at-rules are allowed", and §6.4.4.1 makes the block a conditional group rule
       with a true condition — which is a rule a conditional group rule may contain. */
    if (nested) return type != RULE_TYPE_IMPORT && type != RULE_TYPE_NAMESPACE;
    n = array_len(ctx, list);
    DCHECK(index <= n,
           "§6.4 step 5 was asked about an index past the end of the rule list — step 2 refuses one before the "
           "parse even runs, and this step runs after it");
    /* THE RESULTING LIST, walked once: position k holds the new rule at `index` and the old list either side. */
    for (k = 0; k <= n; k++) {
        uint16_t t = k == index ? type : rule_type_at(ctx, list, k < index ? k : k - 1);

        if (!zone_take(rule_zones(t), &zone_floor)) return false;
    }
    return true;
}

/* §6.4's "list contains anything other than @import at-rules, and @namespace at-rules" — the condition BOTH
   step 6 of insert and step 4 of remove are stated over, so it is one function reached from two places rather
   than two spellings of one sentence.
   IT IS ASKED OF THE ZONE TABLE AND NOT OF A SECOND ENUMERATION, and that is what makes it right for a rule
   CSSOM's sentence could not name. The two at-rules CSSOM lists ARE the sheet prologue as CSSOM knew it, and
   CSS Cascade §6.4.4.2 later put a third rule in that same prologue — "such empty @layer rules are allowed
   before @import and @namespace rules (after the @charset rule, if any)". Reading CSSOM's two names literally
   would refuse `insertRule('@namespace ...')` into a sheet whose own text `@layer a; @import url(x);` PARSES
   as valid, which is the parse and the CSSOM algorithm disagreeing about one fact; the standard that knows
   `@layer` exists is the one that decides it. So the question asked is "can everything already in the list sit
   at or before the `@namespace` zone", which answers CSSOM's own two names identically. */
static bool list_is_prologue_only(JSContext *ctx, JSValueConst list)
{
    const unsigned before = ZONE_BIT(ZONE_LEAD) | ZONE_BIT(ZONE_IMPORT) | ZONE_BIT(ZONE_NAMESPACE);
    uint32_t n = array_len(ctx, list), i;

    for (i = 0; i < n; i++)
        if (!(rule_zones(rule_type_at(ctx, list, i)) & before)) return false;
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
    if (index > array_len(ctx, list))
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
        if (nr->type == RULE_TYPE_NAMESPACE && !list_is_prologue_only(ctx, list)) {
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
    if (index >= array_len(ctx, list))
        return JS_ThrowDOMException(ctx, "IndexSizeError", "the index is at or past the end of the rule list");
    old = JS_GetPropertyUint32(ctx, list, index);        /* STEP 3 */
    DCHECK(css_rule_is(old), "§6.4's remove a CSS rule found something that is not a CSS rule at its index");
    /* STEP 4 — "if old rule is an @namespace at-rule, and list contains anything other than @import at-rules,
       and @namespace at-rules, throw an InvalidStateError". The list is the one the rule is still IN, so the
       test includes the rule being removed, which is why a sheet of nothing but namespaces can lose one. */
    if (css_rule_is(old) && rule_of(old)->type == RULE_TYPE_NAMESPACE &&
        !list_is_prologue_only(ctx, list)) {
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
    uint32_t n = array_len(ctx, kids), i;
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

/* §6.4's CSSStyleRule arm, stated as five steps over three pieces — the PRELUDE, the declaration block and the
   nested rules. Step 2 is §6.6's serialize-a-CSS-declaration-block, which is where the shorthand consolidation
   loop runs; step 3 is this rule's own `cssRules`, which CSS Nesting fills.
   IT IS ALSO §6.4.7's ARM, and that is a derivation rather than a reading: §6.4's CSSPageRule entry is the
   single sentence "need to define how CSSPageRule is serialized", and this is the only arm the spec states for
   a rule whose body holds BOTH declarations and rules — which a page rule's is (page descriptors beside CSS
   Paged Media §4.3's margin at-rules) and which nothing else in §6.4 is. Running it produces `@page { }` for
   `@page {}` and `@page :left { }` for `@page :left {}`, which is what css/cssom/cssom-pagerule.html asserts
   byte for byte and what every engine emits. So the two arms differ only in step 1's `s`, which is the caller's
   to build: a selector list for §6.4.3 and `@page` plus its page selector list for §6.4.7. */
static bool decls_and_rules_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule,
                                      const char *prefix, size_t prefix_len, RBuf *out)
{
    size_t bl = 0;
    char *block, *decls;
    char **kids;
    unsigned nk, i;

    block = rule_text_copy(ctx, r->block_text, &bl);
    decls = block ? cssom_serialize_declarations(block, bl, CSSOM_BLOCK_UNRESTRICTED) : NULL;
    free(block);
    kids = rule_children_serialized(ctx, rule, &nk);
    rbuf_add_n(out, prefix, prefix_len);      /* STEP 1 */
    rbuf_add(out, " {");
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

/* §6.4.3's step 1 is "serialize a group of selectors on the rule's associated selectors", which is the text the
   parse kept and `selectorText =` replaces. */
static bool style_rule_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule, RBuf *out)
{
    size_t sl = 0;
    char *sel = rule_text_copy(ctx, r->selector_text, &sl);
    bool ok;

    DCHECK(sel != NULL,
           "a §6.4.3 style rule has no serialized selector list. Both things that write one — the parse and "
           "`selectorText =` — store only what lexbor accepted, so a null means the string conversion failed");
    if (!sel) return false;
    ok = decls_and_rules_serialize(ctx, r, rule, sel, sl, out);
    free(sel);
    return ok;
}

/* §6.4.7's step 1: `@page`, then — when the rule declares one — a SPACE and the PAGE SELECTOR LIST, which the
   record keeps in the canonical form core/css/css_at_rule_prelude.h produced. The space is inside the
   conditional, which is the whole of why `@page {}` reads back as `@page { }` and not as `@page  { }`. */
static bool page_rule_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule, RBuf *out)
{
    size_t sl = 0;
    char *sel = rule_text_copy(ctx, r->selector_text, &sl);
    RBuf prefix = { NULL, 0, 0 };
    bool ok;

    DCHECK(sel != NULL,
           "a §6.4.7 page rule has no page selector list. `@page {}` declares the EMPTY one, which is the "
           "empty string — both writers store a string and neither stores nothing");
    if (!sel) return false;
    rbuf_add(&prefix, "@page");
    if (sl) { rbuf_add(&prefix, " "); rbuf_add_n(&prefix, sel, sl); }
    free(sel);
    ok = decls_and_rules_serialize(ctx, r, rule, prefix.s, prefix.len, out);
    free(prefix.s);
    return ok;
}

/* §6.4's CSSKeyframesRule arm, WHICH THE SPEC STATES — unlike §6.4.7's and §6.4.8's, which had to be derived.
 * Its five pieces: "@keyframes" and a SPACE; "the serialization of the name attribute. If the attribute is a
 * CSS wide keyword, or the value default, or the value none, then it is serialized as a string. Otherwise, it
 * is serialized as an identifier."; the string " { "; "the result of performing serialize a CSS rule on each
 * rule in the rule's cssRules list, separated by a newline and indented by two spaces"; "a newline, followed
 * by the string "}"".
 * THE ONE PLACE THIS DIVERGES FROM A LITERAL READING IS THE NEWLINE AFTER `{`, and the section says why. Read
 * literally, the fourth piece is a SEPARATOR, so the first child would sit on the brace line
 * (`@keyframes foo {   0% { … }`) and every engine instead emits a newline there; §6.4 attaches its own note
 * to this very arm — "The 'indented by two spaces' bit matches browsers, but needs work, see #5494" — which
 * is the spec deferring to what browsers emit for exactly this piece of exactly this arm. So each child is a
 * newline, two spaces and the child, which is also the shape the CSSMediaRule arm below produces and the shape
 * `@keyframes bar { }` collapses to in css/cssom/CSSKeyframesRule.html's whitespace-insensitive assertion.
 * A KEYFRAMES RULE'S CHILDREN ARE NOT FILTERED FOR EMPTINESS the way §6.4.5's are: this arm names no
 * "filtering out empty strings" step, and it does not need one — every child is a CSSKeyframeRule and every
 * CSSKeyframeRule serializes to at least its keyText and a brace pair.
 * THE SECOND DIVERGENCE IS THE FIRST PIECE, AND IT IS A SECTION THE ARM PREDATES. This arm says "The literal
 * string "@keyframes "" because when it was written that was the only spelling; CSS Compatibility Standard
 * §3.1 "CSS At-rules" then made `@-webkit-keyframes` a second one that "must be supported", and said nothing
 * about how the resulting rule serializes. A literal reading would emit `@keyframes` for a rule the page wrote
 * with the prefix, which is a `cssText` that does not round-trip through the parse it came from; real Chrome
 * 148.0.7778.167 emits the written spelling for exactly that reason. So the piece is the rule's OWN
 * at-keyword, which is the same string the literal names for every rule that was written unprefixed. */
static bool keyframes_rule_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule, RBuf *out)
{
    size_t nl = 0, kwl = 0;
    char *name = rule_text_copy(ctx, r->keyframes_name, &nl);
    char *keyword = rule_text_copy(ctx, r->at_name, &kwl);
    char **kids;
    char *piece;
    unsigned nk, i;

    DCHECK(name != NULL,
           "a CSS Animations §6.3 keyframes rule has no name. §3's grammar has no arm without a "
           "`<keyframes-name>` so the creator refuses a prelude that lacks one, and §6.3.2's setter stores a "
           "string — a null here means the string conversion itself failed");
    DCHECK(keyword != NULL,
           "a CSS Animations §6.3 keyframes rule has no at-keyword. `keyframes_rule_new` is its one creator "
           "and it stores either `keyframes` or the CSS Compatibility §3.1 spelling the page wrote, so a null "
           "here is a rule minted somewhere else — and the serialization would then have to guess which of "
           "the two spellings the page's own stylesheet used");
    if (!name || !keyword) { free(name); free(keyword); return false; }
    /* THE THREE EXCLUSIONS ARE THE `<keyframes-name>` GRAMMAR'S OWN, so they are asked of the one entry that
       holds them (core/css/css_at_rule_prelude.h) rather than restated here — the parse REFUSES exactly this
       set and the serialization QUOTES exactly this set, and that is what makes `cssText` re-parse as the rule
       it came from. A name the parse would have refused reaches this branch because §6.3.2's setter runs no
       grammar, which is what makes `rule.name = 'initial'` read back as `@keyframes "initial"`. */
    piece = css_prelude_keyframes_name_excluded(name) ? css_serialize_string(name, nl)
                                                      : css_serialize_identifier(name, nl);
    free(name);
    kids = rule_children_serialized(ctx, rule, &nk);
    rbuf_add(out, "@");
    rbuf_add(out, keyword);
    rbuf_add(out, " ");
    free(keyword);
    rbuf_add(out, piece);
    free(piece);
    rbuf_add(out, " { \n");
    for (i = 0; i < nk; i++) { rbuf_add_indented(out, kids[i]); rbuf_add(out, "\n"); }
    rbuf_add(out, "}");
    serialized_free(kids, nk);
    return true;
}

/* §6.4's CSSKeyframeRule arm, also stated: "The keyText. The string " { ". The result of performing serialize
   a CSS declaration block on the rule's associated declarations. If the rule is associated with one or more
   declarations, the string " ". The string "}"." So `0% { top: 0px; }`, which css/cssom/CSSKeyframesRule.html
   asserts byte for byte on every rule it touches, and `0% { }` for the block that declares nothing, which is
   what css/cssom/CSSRuleList-appendRule-keyframe-001-crash.html appends.
   THE CONDITIONAL SPACE IS THE WHOLE OF WHY THIS IS NOT THE `@font-face` ARM: there the space belongs to the
   declarations and here it belongs to the closing brace, so an empty block is `0% { }` and not `0% {  }`. */
static bool keyframe_rule_serialize(JSContext *ctx, CssRuleData *r, RBuf *out)
{
    size_t kl = 0, bl = 0;
    char *keys = rule_text_copy(ctx, r->selector_text, &kl);
    char *block, *decls;

    DCHECK(keys != NULL,
           "a CSS Animations §6.2 keyframe rule has no keyText. §3's `<keyframe-selector>#` has no empty arm, "
           "so both writers — the parse and `keyText =` — store a non-empty canonical list or refuse");
    if (!keys) return false;
    block = rule_text_copy(ctx, r->block_text, &bl);
    /* UNRESTRICTED, like every other arm, and that is the point of filtering on the WRITE side: the stored
       text is already what §3 admits, so a serialization that asked again would be a second reader deciding
       for itself — which is the shape css_style_declaration.h's restriction exists to avoid. */
    decls = block ? cssom_serialize_declarations(block, bl, CSSOM_BLOCK_UNRESTRICTED) : NULL;
    free(block);
    rbuf_add_n(out, keys, kl);
    free(keys);
    rbuf_add(out, " { ");
    if (decls) { rbuf_add(out, decls); rbuf_add(out, " "); }
    rbuf_add(out, "}");
    free(decls);
    return true;
}

/* §6.4's CSSMediaRule arm, from its second piece on: a SPACE and "{", a newline, then each nested rule
   (filtering out empty strings, indented by two spaces, joined with newline), a newline and "}".
   THE FINAL NEWLINE BELONGS TO THE ITEMS AND NOT TO THE CLOSING BRACE, which is what makes `@media print {}`
   serialize as "@media print {\n}" rather than "@media print {\n\n}" — the shape every engine produces and the
   one css/cssom/serialize-media-rule.html asserts byte for byte, `@media {}`'s two spaces included.
   IT IS ALSO CSS Cascade §8.1's ARM, AND THAT IS A DERIVATION WITH A NORMATIVE SENTENCE UNDER IT rather than a
   resemblance: §6.4 states no arm for CSSLayerBlockRule at all, and §6.4.4.1 says "such @layer block rules have
   the same restrictions and PROCESSING as a conditional group rule [CSS-CONDITIONAL-3] with a true condition" —
   of which §6.4 states exactly one. It is NOT `decls_and_rules_serialize`'s shape, and the difference is the
   BODY: that arm is for a rule holding declarations BESIDE rules (§6.4.3's and §6.4.7's), and a `@layer`
   block's body is §6.4.4.1's `<rule-list>` with no declarations in it at all. So the two rules differ only in
   step 1's PREFIX, which is the caller's to build. */
static bool group_rules_serialize(JSContext *ctx, JSValueConst rule, const char *prefix, size_t prefix_len,
                                  RBuf *out)
{
    char **kids;
    unsigned nk, i;

    DCHECK(prefix != NULL, "a group rule was serialized with no prefix — every caller writes its at-keyword "
                           "first, so a null is a buffer that was never appended to");
    kids = rule_children_serialized(ctx, rule, &nk);
    rbuf_add_n(out, prefix, prefix_len);
    rbuf_add(out, " {\n");
    for (i = 0; i < nk; i++) { rbuf_add_indented(out, kids[i]); rbuf_add(out, "\n"); }
    rbuf_add(out, "}");
    serialized_free(kids, nk);
    return true;
}

/* HOW MANY `<container-condition>`s the rule declares — the flat Array's length halved, asserted even. */
static uint32_t container_count(JSContext *ctx, CssRuleData *r)
{
    uint32_t n = array_len(ctx, r->container_conditions);

    DCHECK(JS_IsArray(r->container_conditions),
           "an `@container` rule's condition list is not an Array — its one creator builds one before the rule "
           "is handed to anybody, and the freeze means nothing replaces it");
    DCHECK((n % 2) == 0 && n > 0,
           "an `@container` rule's condition list holds an ODD or EMPTY number of strings. It is a flat list "
           "of `name, query` PAIRS, so an odd length is a writer that appended one half of a condition, and "
           "an empty one is a rule the `#` multiplier cannot have produced");
    return n / 2;
}

/* One half of condition `i` — `half` 0 for §9.1's `name` and 1 for its `query`. OWNED. */
static char *container_part(JSContext *ctx, CssRuleData *r, uint32_t i, unsigned half)
{
    JSValue v = JS_GetPropertyUint32(ctx, r->container_conditions, 2 * i + half);
    size_t l = 0;
    char *out = rule_text_copy(ctx, v, &l);

    JS_FreeValue(ctx, v);
    DCHECK(out != NULL,
           "an `@container` rule's condition list holds something that is not a string — its one creator fills "
           "it from core/css/css_at_rule_prelude.h and then FREEZES it, so a null here is the string "
           "conversion itself having failed");
    return out;
}

/* CSS Conditional 5 §9.1's `conditionText`, WHICH IS A CSSContainerRule-SPECIFIC REDEFINITION of §7.2's, given
   there as an algorithm rather than as a stored string: join the conditions with ", ", and within each, emit
   the name if it is not empty, then a single space if the query is also not empty, then the query.
   THAT IS NOT THE SAME AS THE PRELUDE THE PAGE WROTE, and the difference is the point of running the algorithm
   rather than storing the span: the name comes back through CSSOM §2.1's serialize-an-identifier and the
   separators are normalised, so `@container  CARD ( width > 0px ),foo` reads back with one space after the
   name and ", " between the conditions while the query inside each keeps every byte the author typed. OWNED. */
static char *container_condition_text(JSContext *ctx, CssRuleData *r)
{
    uint32_t n = container_count(ctx, r), i;
    RBuf out = { NULL, 0, 0 };

    for (i = 0; i < n; i++) {
        char *name = container_part(ctx, r, i, 0);
        char *query = container_part(ctx, r, i, 1);

        if (!name || !query) { free(name); free(query); free(out.s); return NULL; }
        if (i) rbuf_add(&out, ", ");
        if (name[0]) {
            rbuf_add(&out, name);
            if (query[0]) rbuf_add(&out, " ");
        }
        rbuf_add(&out, query);
        free(name);
        free(query);
    }
    /* An `RBuf` nothing was appended to holds a NULL, and the empty string is not a `conditionText` this rule
       can have — the `!` in §5.4's `<container-condition>` refuses a condition with neither term — so the
       loop above has written at least one byte per condition and there is at least one condition. */
    DCHECK(out.s != NULL,
           "an `@container` rule serialized its condition text to NOTHING. §5.4's `[ <container-name>? "
           "<container-query>? ]!` admits no condition in which both terms are empty, so an empty result "
           "means a rule was built from a prelude the grammar refuses");
    return out.s;
}

/* §6.4's CSSMediaRule arm's step 1: "@media", a SPACE, and the media query list. The space is UNCONDITIONAL
   here where §6.4.7's and §8.1's are not, and the grammars are why: `@media`'s `<media-query-list>` always
   exists (an absent one is the EMPTY list, which is what `@media {}`'s two spaces are), while a page selector
   list and a `<layer-name>` are `?` — optional productions with no separator to write when they are not
   there. */
static bool media_rule_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule, RBuf *out)
{
    char *text = media_list_text(ctx, r->media);
    RBuf prefix = { NULL, 0, 0 };
    bool ok;

    if (!text) return false;
    rbuf_add(&prefix, "@media ");
    rbuf_add(&prefix, text);
    free(text);
    ok = group_rules_serialize(ctx, rule, prefix.s, prefix.len, out);
    free(prefix.s);
    return ok;
}

/* THE `<layer-name>`s A RULE DECLARES, read back out of the frozen Array. `*pn` is how many, and ZERO IS AN
   ANSWER — §6.4.2.1's anonymous layer, which only `@layer { }` has — which is why the FAILURE is the return
   value and not an empty list: a caller that read "no names" out of a string conversion that threw would print
   `@layer {` for a rule that declares one, and that is a plausible datum rather than a crash. False leaves the
   caller's pending exception. OWNED on true: the caller frees the array and its entries. */
static bool rule_layer_names(JSContext *ctx, CssRuleData *r, char ***pv, unsigned *pn)
{
    uint32_t n = array_len(ctx, r->layer_names), i;
    char **out;

    DCHECK(JS_IsArray(r->layer_names),
           "an `@layer` rule's layer name list is not an Array — both creators build one before the rule is "
           "handed to anybody, and nothing replaces it");
    *pv = NULL;
    *pn = 0;
    if (!n) return true;
    out = calloc(n, sizeof(*out));
    CHECK(out != NULL, "cssom: OOM reading an `@layer` rule's layer names");
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, r->layer_names, i);
        size_t l = 0;

        out[i] = rule_text_copy(ctx, v, &l);
        JS_FreeValue(ctx, v);
        DCHECK(out[i] != NULL,
               "an `@layer` rule's layer name list holds something that is not a string — the one creator "
               "fills it from core/css/css_at_rule_prelude.h's serialized names and then FREEZES it, so a null "
               "here means the string conversion itself failed");
        if (!out[i]) { serialized_free(out, i); return false; }
    }
    *pv = out;
    *pn = n;
    return true;
}

/* CSS Cascade §8.1's arm, DERIVED. §6.4 states none, so the pieces come from §6.4.4.1's own grammar
   (`@layer <layer-name>? { <rule-list> }`) laid over the one arm §6.4 does state for a conditional group rule:
   the at-keyword, the name when the rule declares one, and the group body above.
   THE SPACE IS INSIDE THE CONDITIONAL, which is the whole of why `@layer {}` reads back as "@layer {\n}" and
   not as "@layer  {\n}" — the `?` in the grammar means there is no separator to write for a rule that declares
   no name, which is the identical reason §6.4.7's `@page {}` has one space and not two. */
/* CSS Conditional 5 §5.4's `@container`, THROUGH THE SAME ARM AND ON THE SAME DERIVATION AS `@layer`'s. §6.4
   states no arm for CSSContainerRule either — it predates the interface — and §5.4's first sentence supplies
   the one it would have: "The @container rule is a CONDITIONAL GROUP RULE whose condition contains a container
   query", and §6.4 states exactly one arm for a conditional group rule. So this differs from the `@media` arm
   above it only in step 1's PREFIX.
   THE PREFIX IS THE `conditionText` ALGORITHM AND NOT THE STORED PRELUDE, which is what makes `cssText`
   re-parse as the rule it came from: §9.1 defines that algorithm to reproduce a `<container-condition>#`, name
   through CSSOM §2.1's serialize-an-identifier and conditions joined by §2.1's ", ", so running it is how the
   serialization and the attribute cannot disagree about one rule. */
static bool container_rule_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule, RBuf *out)
{
    RBuf prefix = { NULL, 0, 0 };
    char *text = container_condition_text(ctx, r);
    bool ok;

    if (!text) return false;
    rbuf_add(&prefix, "@container ");
    rbuf_add(&prefix, text);
    free(text);
    ok = group_rules_serialize(ctx, rule, prefix.s, prefix.len, out);
    free(prefix.s);
    return ok;
}

/* CSS Conditional §7.4's arm, DERIVED the same way CSS Cascade §8.1's below it is and from the same place:
   CSSOM §6.4's serialize-a-CSS-rule states no arm for CSSSupportsRule at all — its list runs CSSStyleRule,
   CSSImportRule, CSSMediaRule, CSSFontFaceRule, CSSPageRule, CSSNamespaceRule, CSSKeyframesRule,
   CSSKeyframeRule and stops — so the shape comes from the one arm it DOES state for a conditional group rule,
   §7.3's, with step 1's prefix replaced. The prefix is the at-keyword, a SPACE and the condition: `@supports`'s
   `<supports-condition>` is not an optional production (`@supports {}` matches no arm of §6's grammar and is
   dropped before it can be serialized), so the space is unconditional exactly as `@media`'s is. */
static bool supports_rule_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule, RBuf *out)
{
    RBuf prefix = { NULL, 0, 0 };
    const char *text = JS_ToCString(ctx, r->supports_text);
    bool ok;

    if (!text) return false;
    rbuf_add(&prefix, "@supports ");
    rbuf_add(&prefix, text);
    JS_FreeCString(ctx, text);
    ok = group_rules_serialize(ctx, rule, prefix.s, prefix.len, out);
    free(prefix.s);
    return ok;
}

static bool layer_block_rule_serialize(JSContext *ctx, CssRuleData *r, JSValueConst rule, RBuf *out)
{
    RBuf prefix = { NULL, 0, 0 };
    char **names;
    unsigned n;
    bool ok;

    if (!rule_layer_names(ctx, r, &names, &n)) return false;
    DCHECK(n <= 1, "a §8.1 layer block rule declares more than one `<layer-name>` — §6.4.4.1's grammar is "
                   "`<layer-name>?`, and its creator refuses a prelude carrying a list");
    rbuf_add(&prefix, "@layer");
    if (n) { rbuf_add(&prefix, " "); rbuf_add(&prefix, names[0]); }
    serialized_free(names, n);
    ok = group_rules_serialize(ctx, rule, prefix.s, prefix.len, out);
    free(prefix.s);
    return ok;
}

/* CSS Cascade §8.2's arm, DERIVED the same way and from the same place: §6.4 states none, so the pieces are
   §6.4.4.2's own grammar `@layer <layer-name>#;` — the at-keyword, a SPACE, the names through CSSOM §2.1's
   serialize-a-comma-separated-list (", "), and the SEMICOLON that makes it a statement at-rule. The space is
   unconditional here and conditional in the arm above because the multipliers differ: `#` has no zero-length
   arm, so a statement rule always declares at least one name and its creator refuses a prelude that does not. */
static bool layer_statement_rule_serialize(JSContext *ctx, CssRuleData *r, RBuf *out)
{
    char **names;
    unsigned n, i;

    if (!rule_layer_names(ctx, r, &names, &n)) return false;
    DCHECK(n >= 1, "a §8.2 layer statement rule declares NO `<layer-name>` — §6.4.4.2's `#` multiplier has no "
                   "zero-length arm, and its creator refuses a prelude with no name in it");
    rbuf_add(out, "@layer ");
    for (i = 0; i < n; i++) {
        if (i) rbuf_add(out, ", ");
        rbuf_add(out, names[i]);
    }
    serialized_free(names, n);
    rbuf_add(out, ";");
    return true;
}

/* CSS Properties and Values API 1 §6.1's `name` — "the custom property name associated with the @property
 * rule" — read out of the LIST §3's prelude declares. It is ONE reader because §6.1's `name` attribute and
 * §6.1's serialization arm ask the identical question, and because that question has an unresolved answer for
 * one shape of rule, which must therefore be stated once. OWNED (a string). */
static JSValue property_rule_name(JSContext *ctx, CssRuleData *r)
{
    DCHECK(JS_IsArray(r->property_names),
           "an `@property` rule's custom property name list is not an Array — the one creator builds one before "
           "the rule is handed to anybody, and nothing replaces it");
    DCHECK(array_len(ctx, r->property_names) == 1,
           "an `@property` rule declares SEVERAL custom property names and §6.1 gives the interface ONE `name`. "
           "That is not a gap in this build: §3 admits the list ('a valid @property rule represents a custom "
           "property registration for EACH <custom-property-name> in the rule's prelude') and §6.1 carries the "
           "CSSWG's own note that 'the CSSOM for multi-name @property rules has not been resolved on' "
           "(w3c/csswg-drafts issue #14227). So the rule is VALID and the ATTRIBUTE has no defined answer — "
           "there is nothing to invent, and picking the first name would be a value indistinguishable from a "
           "computed one. Build whatever that resolution says, HERE, which is the one place both `name` and "
           "§6.1's serialization read");
    return JS_GetPropertyUint32(ctx, r->property_names, 0);
}

/* CSS Properties and Values API 1 §6.1's OWN SERIALIZATION ARM, which that section states in full — unlike
 * §6.4.7's and §6.4.8's, which had to be derived. Its pieces, in order: `"@property"` and a SPACE; serialize an
 * identifier on the rule's name and a SPACE; the string `"{ "`; `"syntax:"` and a SPACE; serialize a string on
 * the rule's syntax, a SEMICOLON and a SPACE; `"inherits:"` and a SPACE; `"true"` or `"false"` by the
 * attribute's value, a SEMICOLON and a SPACE; then, IF the initial-value is present, `"initial-value:"`,
 * serialize a CSS value on it, a SEMICOLON and a SPACE; then a RIGHT CURLY BRACKET.
 *
 * THE ONE PLACE THIS DIVERGES FROM THE STEP AS WRITTEN IS THE SPACE AFTER `initial-value:`. That step names the
 * string `"initial-value:"` and stops, where its two siblings four and six each name the descriptor name AND
 * "a single SPACE (U+0020)" — and the platform emits the space: css/css-properties-values-api/
 * at-property-cssom.html pins `@property --valid { syntax: "<color> | none"; inherits: false; initial-value:
 * red; }` byte for byte, so the omission is an editorial slip in one step of one arm rather than a difference
 * anybody implements. The three descriptors are emitted in the SECTION'S order and never the author's, which is
 * what that same test's `--valid-reverse` reads back from a rule written initial-value first.
 *
 * `serialize a CSS value` OVER A TOKEN STREAM IS THE STREAM. §3.3 types the descriptor `<declaration-value>?`,
 * which has no parsed form to re-serialize from — the value is whatever tokens the author wrote — so what is
 * emitted is what was declared, which is also what makes `initial-value: red, blue` come back with its comma. */
static bool property_rule_serialize(JSContext *ctx, CssRuleData *r, RBuf *out)
{
    JSValue name_val = property_rule_name(ctx, r);
    size_t nl = 0, sl = 0, il = 0;
    char *name = rule_text_copy(ctx, name_val, &nl);
    char *syntax, *initial, *piece;

    JS_FreeValue(ctx, name_val);
    DCHECK(name != NULL,
           "an `@property` rule has no name. §3's `<custom-property-name>#` has no arm without one and the one "
           "creator refuses a prelude that lacks one, so a null here means the string conversion itself failed");
    if (!name) return false;
    syntax = rule_text_copy(ctx, r->property_syntax, &sl);
    DCHECK(syntax != NULL,
           "an `@property` rule has no syntax. §3.1's descriptor is OPTIONAL and its INITIAL is `\"*\"`, which "
           "the creator stores for a rule that declares none — so the field is a string on every `@property` "
           "rule there is and a null here is the conversion failing");
    if (!syntax) { free(name); return false; }
    DCHECK(JS_IsBool(r->property_inherits),
           "an `@property` rule's inherit flag is not a boolean — §3.2's descriptor is OPTIONAL with the INITIAL "
           "`true`, so the creator stores one of the two on every rule and there is no third state to reach");
    rbuf_add(out, "@property ");
    piece = css_serialize_identifier(name, nl);
    rbuf_add(out, piece);
    free(piece);
    free(name);
    rbuf_add(out, " { syntax: ");
    piece = css_serialize_string(syntax, sl);
    rbuf_add(out, piece);
    free(piece);
    free(syntax);
    rbuf_add(out, "; inherits: ");
    rbuf_add(out, JS_ToBool(ctx, r->property_inherits) ? "true" : "false");
    rbuf_add(out, "; ");
    /* "If the rule's initial-value is present" — §3.3's initial is the guaranteed-invalid value, which is this
       field's JS_NULL and is exactly the absence this step tests. */
    initial = rule_text_copy(ctx, r->property_initial_value, &il);
    if (initial) {
        rbuf_add(out, "initial-value: ");
        rbuf_add(out, initial);
        rbuf_add(out, "; ");
        free(initial);
    }
    rbuf_add(out, "}");
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

/* §6.4's CSSFontFaceRule arm, and §6.4.8's, which is the same arm. The spec's own font-face steps name each
   descriptor in a fixed order and then admit "need to define how the CSSFontFaceRule descriptors' values are
   serialized"; every step has the SAME shape — a SPACE, `name:`, a SPACE, the value, `;` — which is exactly
   what §6.6's serialize-a-CSS-declaration-block produces for the block once the leading space and the closing
   " }" are added. So the descriptors go through the ONE declaration-block serializer rather than through a
   second hand-listed loop that could disagree with it about `rule.style.cssText`, and the order is the rule's
   own (which is what Blink and WebKit report, and what css/cssom/CSSFontFaceRule.html declines to pin because
   engines differ).
   §6.4 STATES NO ARM AT ALL FOR §6.4.8's CSSMarginRule, and this is it because a margin at-rule's body is CSS
   Paged Media §4.3's `<declaration-list>` — declarations and nothing else, which is the shape this arm IS. The
   at-keyword is the only difference, so it is a parameter: `@font-face` and `@top-left` are one algorithm over
   two names, and a second copy could only disagree about the spacing. */
static bool decl_body_rule_serialize(JSContext *ctx, CssRuleData *r, const char *at_name, RBuf *out)
{
    size_t bl = 0;
    char *block = rule_text_copy(ctx, r->block_text, &bl);
    char *decls;

    DCHECK(block != NULL,
           "a rule whose body is a declaration list has no declaration text. `@font-face {}` and `@top-left {}` "
           "declare nothing, which is the EMPTY STRING, so a null here means the string conversion itself "
           "failed");
    if (!block) return false;
    decls = cssom_serialize_declarations(block, bl, CSSOM_BLOCK_UNRESTRICTED);
    free(block);
    rbuf_add(out, "@");
    rbuf_add(out, at_name);
    rbuf_add(out, " {");
    if (decls) { rbuf_add(out, " "); rbuf_add(out, decls); }
    rbuf_add(out, " }");
    free(decls);
    return true;
}

/* §6.4.8's `name` as the at-keyword it serializes back to. It is the record's own string, so a margin rule
   whose name is missing is this file disagreeing with its own creator rather than a shape a page can make. */
static bool margin_rule_serialize(JSContext *ctx, CssRuleData *r, RBuf *out)
{
    size_t nl = 0;
    char *name = rule_text_copy(ctx, r->at_name, &nl);
    bool ok;

    DCHECK(name != NULL,
           "a §6.4.8 margin rule has no at-rule name. `margin_rule_new` is the one creator and it stores one "
           "of CSS Paged Media §4.3's sixteen, so a null here means the string conversion itself failed");
    if (!name) return false;
    ok = decl_body_rule_serialize(ctx, r, name, out);
    free(name);
    return ok;
}

static bool rule_serialize(JSContext *ctx, JSValueConst rule, RBuf *out)
{
    CssRuleData *r = rule_of(rule);

    DCHECK(r != NULL, "§6.4's serialize a CSS rule was invoked on something that is not a CSS rule");
    if (!r) return false;
    switch (r->type) {
    case RULE_TYPE_MEDIA:     return media_rule_serialize(ctx, r, rule, out);
    case RULE_TYPE_SUPPORTS:  return supports_rule_serialize(ctx, r, rule, out);
    case RULE_TYPE_CONTAINER: return container_rule_serialize(ctx, r, rule, out);
    case RULE_TYPE_IMPORT:    return import_rule_serialize(ctx, r, out);
    case RULE_TYPE_NAMESPACE: return namespace_rule_serialize(ctx, r, out);
    case RULE_TYPE_FONT_FACE: return decl_body_rule_serialize(ctx, r, "font-face", out);
    case RULE_TYPE_PAGE:      return page_rule_serialize(ctx, r, rule, out);
    case RULE_TYPE_MARGIN:    return margin_rule_serialize(ctx, r, out);
    case RULE_TYPE_KEYFRAMES: return keyframes_rule_serialize(ctx, r, rule, out);
    case RULE_TYPE_KEYFRAME:  return keyframe_rule_serialize(ctx, r, out);
    case RULE_TYPE_LAYER_BLOCK:     return layer_block_rule_serialize(ctx, r, rule, out);
    case RULE_TYPE_LAYER_STATEMENT: return layer_statement_rule_serialize(ctx, r, out);
    case RULE_TYPE_PROPERTY:        return property_rule_serialize(ctx, r, out);
    default:
        DCHECK(r->type == RULE_TYPE_STYLE, "§6.4's serialize a CSS rule met a rule type it has no arm for");
        return style_rule_serialize(ctx, r, rule, out);
    }
}

/* ---- the members ------------------------------------------------------------------------------------------ */

enum { CR_PARENT_RULE = 0, CR_PARENT_STYLE_SHEET, CR_TYPE, CR_CSS_TEXT, CR_SELECTOR_TEXT, CR_CONDITION_TEXT,
       CR_MEDIA, CR_MATCHES, CR_SUPPORTS_MATCHES, CR_CSS_RULES, CR_HREF, CR_IMPORT_MEDIA, CR_LAYER_NAME,
       CR_SUPPORTS_TEXT,
       CR_NAMESPACE_URI, CR_PREFIX, CR_PAGE_SELECTOR_TEXT, CR_MARGIN_NAME, CR_KEY_TEXT, CR_KEYFRAMES_NAME,
       CR_KEYFRAMES_CSS_RULES, CR_KEYFRAMES_LENGTH, CR_LAYER_BLOCK_NAME, CR_LAYER_NAME_LIST,
       CR_PROPERTY_NAME, CR_PROPERTY_SYNTAX, CR_PROPERTY_INHERITS, CR_PROPERTY_INITIAL_VALUE,
       CR_CONTAINER_NAME, CR_CONTAINER_QUERY, CR_CONTAINER_CONDITIONS };

/* §6.4.5's `[SameObject] cssRules` and CSS Animations §6.3.2's, which are one read of one Array. The
   collection is remembered on the record because both are [SameObject], and it SHARES the very Array the
   children live in, which is what its liveness IS. Two attributes of two interfaces, one body: what differs is
   the BRAND their getters check, which is the caller's. */
static JSValue rule_css_rules(JSContext *ctx, CssRuleData *r)
{
    if (!JS_IsObject(r->rule_list))
        rule_set(ctx, r, &r->rule_list, css_rule_list_new(ctx, JS_DupValue(ctx, r->child_rules)));
    return JS_DupValue(ctx, r->rule_list);
}

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
       is created and cannot change" — mapped through the frozen table, because past its end the answer §6.4.2
       states is 0 and not a number this file could invent. `layerRule.type === 0`, and the spec's own note
       says what a page should read instead: "to tell what type of rule a given object is, it is recommended to
       check rule.constructor.name". */
    case CR_TYPE:
        r = rule_here(ctx, this_val);
        return r ? JS_NewUint32(ctx, rule_legacy_type(r->type)) : JS_EXCEPTION;
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
    /* CSS Conditional §7.2's `conditionText`, WHICH EACH DERIVED INTERFACE REDEFINES — §7.2 says so outright
       ("Since what this condition does varies between the derived interfaces of CSSConditionRule, those
       derived interfaces may specify different behavior for this attribute") and then both of them do, so this
       is one member with THREE definitions and not one definition with three receivers.
       §7.3's, for a `@media`: "must return the value of media.mediaText on the rule" — not a second copy of
       the condition but ONE read of the MediaList, which is where a media rule's condition lives.
       §7.4's, for a `@supports`: "must return the condition that was specified, without any logical
       simplifications" — the stored prelude, for the reason supports_rule_new gives.
       CSS Conditional 5 §9.1's, for an `@container`: an ALGORITHM over the rule's `conditions` rather than any
       stored string, which is the third shape again — see `container_condition_text`.
       The BRAND is CSSConditionRule's, so a `@supports` reaching `CSSMediaRule.prototype.conditionText` is the
       same TypeError a style rule gets: both conditional types answer here and nothing else does. */
    case CR_CONDITION_TEXT: {
        char *text;
        JSValue out;

        r = rule_of(this_val);
        if (!r || (r->type != RULE_TYPE_MEDIA && r->type != RULE_TYPE_SUPPORTS &&
                   r->type != RULE_TYPE_CONTAINER)) {
            JS_ThrowTypeError(ctx, "a CSSConditionRule member was reached on something that is not a "
                                   "CSSConditionRule");
            return JS_EXCEPTION;
        }
        if (r->type == RULE_TYPE_SUPPORTS) return JS_DupValue(ctx, r->supports_text);
        text = r->type == RULE_TYPE_CONTAINER ? container_condition_text(ctx, r)
                                              : media_list_text(ctx, r->media);
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
    /* CSS Conditional §7.4's `matches`: "The matches attribute returns the evaluation of the CSS feature query
       represented in conditionText." That is the WHOLE definition, and the difference from §7.3's one member
       up is the whole reason this is a second magic rather than a second receiver on that one: §7.3 conjoins
       "the rule is in a stylesheet attached to a document" and §7.4 states no such conjunct, so a `@supports`
       rule removed from its sheet still answers its condition.
       AND IT IS CONCRETE WHERE §7.3's IS CONCOLIC. A media query asks about an environment this headless
       engine does not have, so its answer forks the alternate-viewport world; a feature query asks whether
       THIS user agent accepts a declaration, which is a fact about the program doing the asking. Answering it
       symbolically would fork a world that cannot exist — see core/css/css_supports.h. */
    case CR_SUPPORTS_MATCHES:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_SUPPORTS, "CSSSupportsRule");
        return r ? JS_NewBool(ctx, rule_supports_matches(ctx, r)) : JS_EXCEPTION;
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
    /* §6.4.7: "The selectorText attribute, on getting, must return the result of serializing the associated
       selector list" — a PAGE selector list, whose grammar and serialization are CSS Paged Media §4.3's and
       not Selectors', which is why this is a second attribute rather than §6.4.3's reached from two
       prototypes. The record holds the serialization the parse (or the setter) already produced. */
    case CR_PAGE_SELECTOR_TEXT:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_PAGE, "CSSPageRule");
        return r ? JS_DupValue(ctx, r->selector_text) : JS_EXCEPTION;
    /* §6.4.8: "The name attribute must return the name of the margin at-rule. The @ character is not included
       in the name." */
    case CR_MARGIN_NAME:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_MARGIN, "CSSMarginRule");
        return r ? JS_DupValue(ctx, r->at_name) : JS_EXCEPTION;
    /* CSS Animations §6.2.2: "This attribute represents the keyframe selector as a comma-separated list of
       percentage values. The from and to keywords map to 0% and 100%, respectively." The record holds that
       canonical list — core/css/css_at_rule_prelude.h produced it, at the parse and at the setter alike —
       which is why `from` reads back as `0%`. */
    case CR_KEY_TEXT:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_KEYFRAME, "CSSKeyframeRule");
        return r ? JS_DupValue(ctx, r->selector_text) : JS_EXCEPTION;
    /* CSS Animations §6.3.2: "This attribute is the name of the keyframes, used by the animation-name
       property." The name AS WRITTEN, which is not how §6.4 serializes it — see the serialization arm. */
    case CR_KEYFRAMES_NAME:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_KEYFRAMES, "CSSKeyframesRule");
        return r ? JS_DupValue(ctx, r->keyframes_name) : JS_EXCEPTION;
    /* CSS Animations §6.3.2: "This attribute gives access to the keyframes in the list." It is CSSKeyframesRule's
       OWN attribute and not §6.4.5's — a `@keyframes` is not a CSSGroupingRule — so it brand-checks against
       this interface and shares the one body with it. */
    case CR_KEYFRAMES_CSS_RULES:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_KEYFRAMES, "CSSKeyframesRule");
        return r ? rule_css_rules(ctx, r) : JS_EXCEPTION;
    /* CSS Animations §6.3.2: "This attribute is the number of keyframes in the list." It is the length of the
       child list itself and never a second count, which is also what makes it the `length` Web IDL §3.7.10
       pairs with the indexed getter below. */
    case CR_KEYFRAMES_LENGTH:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_KEYFRAMES, "CSSKeyframesRule");
        return r ? JS_NewUint32(ctx, array_len(ctx, r->child_rules)) : JS_EXCEPTION;
    /* CSS Cascade §8.1: "Its name attribute represents the layer name declared by the at-rule ITSELF, and is
       an empty string if the layer is anonymous." The emphasis is §8.1's own, and its worked example is what
       it buys: inside `@layer outer { @layer foo.bar { } }` "the name of the inner @layer rule is 'foo.bar'
       (and not 'outer.foo.bar')" — so this is the rule's own prelude and never a concatenation with the
       enclosing layers', which is also why nothing here walks `parent_rule`.
       It is index 0 of the ONE list both interfaces answer from: §6.4.4.1's `<layer-name>?` is a list of at
       most one, and an EMPTY one is §6.4.2.1's anonymous layer — which is where the empty string comes from,
       so the two are one storage and not a string beside a list that could disagree. */
    case CR_LAYER_BLOCK_NAME: {
        JSValue first;

        r = rule_here_typed(ctx, this_val, RULE_TYPE_LAYER_BLOCK, "CSSLayerBlockRule");
        if (!r) return JS_EXCEPTION;
        first = JS_GetPropertyUint32(ctx, r->layer_names, 0);
        if (!JS_IsUndefined(first)) return first;
        JS_FreeValue(ctx, first);
        return JS_NewString(ctx, "");
    }
    /* CSS Cascade §8.2: "Its nameList attribute represents the list of layer names declared by the at-rule,
       NORMALIZED FOLLOWING THE SAME RULE as the CSSLayerBlockRule's name attribute." One normalization is
       therefore one storage, and core/css/css_at_rule_prelude.h is where it happens — for both at-rules, out
       of one grammar. The Array is the record's own and is already FROZEN, which is what a `FrozenArray<T>`
       VALUE is (Web IDL §2.13.35), so this is a read of a reference and not a per-read conversion. */
    case CR_LAYER_NAME_LIST:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_LAYER_STATEMENT, "CSSLayerStatementRule");
        return r ? JS_DupValue(ctx, r->layer_names) : JS_EXCEPTION;
    /* CSS Properties and Values API 1 §6.1: "name, of type CSSOMString, readonly — The custom property name
       associated with the @property rule." UNSERIALIZED, which is the difference from `cssText`: §6.1's own
       serialization arm performs serialize-an-identifier on this value, so what the attribute returns is the
       name itself (`--tab\ttab` for a rule written `--tab\9 tab`) and the escaping belongs to the other reader.
       It is the same split CSS Animations §6.3.2's `name` has and for the same reason. */
    case CR_PROPERTY_NAME:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_PROPERTY, "CSSPropertyRule");
        return r ? property_rule_name(ctx, r) : JS_EXCEPTION;
    /* §6.1: "syntax, of type CSSOMString, readonly — The syntax associated with the @property, EXACTLY AS
       SPECIFIED." So it is the `<string>`'s own value with nothing trimmed — `" <color># "` reads back with its
       spaces — and it is §3.1's initial `"*"` for a rule that declares no syntax or declares one §5.4.2 refuses,
       which is that section's "the descriptor is invalid and must be ignored" and not a stand-in. */
    case CR_PROPERTY_SYNTAX:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_PROPERTY, "CSSPropertyRule");
        return r ? JS_DupValue(ctx, r->property_syntax) : JS_EXCEPTION;
    /* §6.1: "inherits, of type boolean, readonly — The inherits descriptor associated with the @property
       rule." §3.2's initial is `true`, which the creator stores for a rule that declares none. */
    case CR_PROPERTY_INHERITS:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_PROPERTY, "CSSPropertyRule");
        if (!r) return JS_EXCEPTION;
        DCHECK(JS_IsBool(r->property_inherits),
               "§6.1 types `inherits` a boolean and the record holds something else — §3.2's descriptor is "
               "optional with an INITIAL, so every `@property` rule carries one of the two flags");
        return JS_DupValue(ctx, r->property_inherits);
    /* §6.1: "initialValue, of type CSSOMString, readonly, nullable — The initial value associated with the
       @property rule, WHICH MAY NOT BE PRESENT." The null is §3.3's own initial (the guaranteed-invalid value)
       and is therefore a real answer rather than an absence this getter has to invent. */
    case CR_PROPERTY_INITIAL_VALUE:
        r = rule_here_typed(ctx, this_val, RULE_TYPE_PROPERTY, "CSSPropertyRule");
        return r ? JS_DupValue(ctx, r->property_initial_value) : JS_EXCEPTION;
    /* CSS Conditional 5 §9.1's `containerName` and `containerQuery`, WHICH ANSWER "" FOR A RULE THAT DECLARES
       MORE THAN ONE CONDITION and that is the definition rather than a shortfall: both are "if the length of
       conditions is 1: return the only conditions item's name/query", "return ''" otherwise. §9.1's own note
       says why they are shaped that way — "we should try to remove containerName and containerQuery, since
       they don't deal with multiple conditions correctly" — so `conditions` is the member that carries the
       whole truth and these two are the legacy pair, kept because pages read them.
       THE EMPTY STRING IS THEREFORE THREE DIFFERENT FACTS AT ONE MEMBER (no name declared, more than one
       condition, or — for the query — a bare `<container-name>` with no query) and §9.1 collapses all three
       deliberately. Nothing here may un-collapse them: `conditions` is where a caller that needs them apart
       looks, which is the whole reason it exists. */
    case CR_CONTAINER_NAME:
    case CR_CONTAINER_QUERY: {
        char *part;
        JSValue out;

        r = rule_here_typed(ctx, this_val, RULE_TYPE_CONTAINER, "CSSContainerRule");
        if (!r) return JS_EXCEPTION;
        if (container_count(ctx, r) != 1) return JS_NewString(ctx, "");
        part = container_part(ctx, r, 0, magic == CR_CONTAINER_NAME ? 0u : 1u);
        if (!part) return JS_EXCEPTION;
        out = JS_NewString(ctx, part);
        free(part);
        return out;
    }
    /* §9.1's `conditions` — "let result be an empty list … append dict to result … return result", MINTED PER
       GET. It is not `[SameObject]`, and the algorithm builds a new list of new `CSSContainerCondition`
       dictionaries every time it runs, so handing back a stored value would answer `===` the way the platform
       does not AND would let a page write into the rule's own record (Web IDL §3.2.27 freezes the ARRAY, never
       the dictionaries in it). The array itself IS frozen, which is that same section: "perform
       SetIntegrityLevel(array, "frozen")". */
    case CR_CONTAINER_CONDITIONS: {
        uint32_t n, i;
        JSValue a;

        r = rule_here_typed(ctx, this_val, RULE_TYPE_CONTAINER, "CSSContainerRule");
        if (!r) return JS_EXCEPTION;
        n = container_count(ctx, r);
        a = JS_NewArray(ctx);
        if (JS_IsException(a)) return a;
        for (i = 0; i < n; i++) {
            char *name = container_part(ctx, r, i, 0);
            char *query = container_part(ctx, r, i, 1);
            JSValue dict = JS_NewObject(ctx);

            if (!name || !query || JS_IsException(dict)) {
                free(name); free(query); JS_FreeValue(ctx, dict); JS_FreeValue(ctx, a);
                return JS_EXCEPTION;
            }
            /* Both members are `required` in §9.1's dictionary, so each is present on every entry and neither
               is ever the absence the empty string would otherwise be mistaken for. */
            JS_SetPropertyStr(ctx, dict, "name", JS_NewString(ctx, name));
            JS_SetPropertyStr(ctx, dict, "query", JS_NewString(ctx, query));
            free(name);
            free(query);
            JS_SetPropertyUint32(ctx, a, i, dict);
        }
        if (idl_freeze_array(ctx, a) != 0) { JS_FreeValue(ctx, a); return JS_EXCEPTION; }
        return a;
    }
    /* §6.4.5: "The cssRules attribute must return a CSSRuleList object for the child CSS rules." [SameObject],
       so the collection is remembered on the record — and it shares the very Array the children live in, which
       is what its liveness IS. */
    default:
        DCHECK(magic == CR_CSS_RULES, "a CSS rule attribute ran with a magic §6.4 does not declare");
        r = rule_here_grouping(ctx, this_val);
        return r ? rule_css_rules(ctx, r) : JS_EXCEPTION;
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
   NULL, which is the setter's "the algorithm returned null, do nothing" — and a qualified rule whose prelude
   was NOT accepted as a selector list is exactly that case, which is why the probe asks. Without that test
   `rule.selectorText = '0%'` would store the raw `0%` the parse now reports for a keyframe block's prelude. */
static void *css_rule_selector_probe(void *ud, void *parent, const CssomRule *pr)
{
    char **out = ud;

    if (parent || pr->at_name || !pr->prelude_is_selectors || *out || !pr->prelude || !*pr->prelude)
        return NULL;
    *out = strdup(pr->prelude);
    CHECK(*out != NULL, "cssom: OOM keeping a parsed selector list");
    return NULL;
}

/* CSS Syntax's PARSE A GROUP OF SELECTORS over a span of text: the CANONICAL SERIALIZATION lexbor produced for
   it, or NULL when the text is not a selector list at all. OWNED: the caller frees.
   THE GROUP OF SELECTORS IS PARSED BY PARSING A RULE WITH AN EMPTY BODY, because that is the one entry the
   agent's parser exposes and because it answers exactly the question §6.4.3 asks: a value lexbor accepts as a
   selector list comes back as one style rule whose serialization is the canonical form the getter must then
   return, and a value it rejects produces no style rule at all.
   IT IS ONE FUNCTION BECAUSE TWO CALLERS ASK THE SAME QUESTION FOR OPPOSITE REASONS. §6.4.3's setter must not
   STORE a value the parser rejected; the author cascade must not EMIT one — the flattened sheet is re-parsed
   and core/css/css_style_declaration.c asserts the emission against that parse AT EVERY INDEX, so a rule whose
   text comes back as a different kind of rule shifts every rule after it into a neighbour's cascade layer. */
static char *selector_list_reserialize(const char *sel, size_t len)
{
    char *reserialized = NULL, *probe;
    unsigned n;

    DCHECK(sel != NULL, "a group of selectors was parsed from no text at all");
    probe = malloc(len + 4);
    CHECK(probe != NULL, "cssom: OOM parsing a selector list");
    memcpy(probe, sel, len);
    memcpy(probe + len, "{}", 3);
    n = cssom_parse_rules(probe, len + 2, css_rule_selector_probe, &reserialized);
    free(probe);
    if (n == 1 && reserialized) return reserialized;
    free(reserialized);
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
    char *reserialized;

    (void)magic;
    if (!r) return JS_EXCEPTION;
    v = JS_ToCString(ctx, val);   /* a real string by now: the declaration converted it */
    if (!v) return JS_EXCEPTION;
    reserialized = selector_list_reserialize(v, strlen(v));
    JS_FreeCString(ctx, v);
    if (reserialized) {
        /* A NESTED RULE'S SELECTOR STAYS ABSOLUTIZED THROUGH A WRITE, because that is what every reader of it
           is entitled to assume: CSS Nesting §6 "CSSOM" absolutizes what a nested rule serializes, and
           core/css/css_nesting.h's resolve asserts the same premise from the other side, so storing a bare
           `.bar` here would leave a nested rule whose text names no parent at all.
           WHAT IS NOT BUILT IS THE RELATIVE HALF OF THIS SETTER, and it is named rather than approximated:
           §6.4.3's algorithm is "parse a group of selectors", which for a nested rule is a
           `<relative-selector-list>`, and the parser above implements neither the nesting selector nor a
           leading combinator — so `nested.selectorText = '&:hover'` takes §6.4.3's OWN null branch ("do
           nothing") where a browser accepts it. The capability to build is a relative-selector-list parse;
           until it exists this write is refused rather than stored unparsed. */
        if (!JS_IsNull(rule_nesting_parent(r->parent_rule))) {
            char *absolutized = css_nesting_absolutize(reserialized, strlen(reserialized));

            free(reserialized);
            reserialized = absolutized;
        }
        rule_set(ctx, r, &r->selector_text, JS_NewString(ctx, reserialized));
    }
    free(reserialized);
    return JS_UNDEFINED;
}

/* §6.4.7's setter, whose three steps are §6.4.3's three with ONE algorithm swapped: "run the PARSE A LIST OF
   CSS PAGE SELECTORS algorithm on the given value. If the algorithm returns a non-null value replace the
   associated selector list with the returned value. Otherwise, if the algorithm returns a null value, DO
   NOTHING." So an invalid page selector is silently ignored — `rule.selectorText = ':notapagepseudo'` leaves
   the rule exactly as it was, which css/cssom/cssom-pagerule.html asserts four times over — and the EMPTY
   STRING is not that case: it parses to the empty list, which is a non-null value, so `selectorText = ''`
   really does clear the list.
   IT IS A SECOND SETTER AND NOT A BRANCH IN THE ONE ABOVE, because the two attributes are two members of two
   interfaces over two grammars: a page selector list is CSS Paged Media §4.3's and is not a group of selectors
   at all — `named:first` is one and no Selectors production admits it. */
static JSValue js_rule_set_page_selector(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    CssRuleData *r = rule_here_typed(ctx, this_val, RULE_TYPE_PAGE, "CSSPageRule");
    const char *v;
    char *parsed;

    (void)magic;
    if (!r) return JS_EXCEPTION;
    v = JS_ToCString(ctx, val);   /* a real string by now: the declaration converted it */
    if (!v) return JS_EXCEPTION;
    parsed = css_prelude_page_selectors(v, strlen(v));
    JS_FreeCString(ctx, v);
    if (parsed) {
        rule_set(ctx, r, &r->selector_text, JS_NewString(ctx, parsed));
        free(parsed);
    }
    return JS_UNDEFINED;
}

/* CSS Animations §6.2.2's setter, and it is neither of the two above. "If keyText is updated with an INVALID
   keyframe selector, a SyntaxError exception must be THROWN and the value of keyText must remain unchanged" —
   where §6.4.3's and §6.4.7's both do nothing at all. Three attributes, three sentences, three bodies; folding
   them would have to carry the difference in a flag, and the flag is the sentence. */
static JSValue js_rule_set_key_text(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    CssRuleData *r = rule_here_typed(ctx, this_val, RULE_TYPE_KEYFRAME, "CSSKeyframeRule");
    const char *v;
    char *parsed;

    (void)magic;
    if (!r) return JS_EXCEPTION;
    v = JS_ToCString(ctx, val);   /* a real string by now: the declaration converted it */
    if (!v) return JS_EXCEPTION;
    parsed = css_prelude_keyframe_selectors(v, strlen(v));
    JS_FreeCString(ctx, v);
    if (!parsed)
        return JS_ThrowDOMException(ctx, "SyntaxError", "the value is not a keyframe selector list");
    rule_set(ctx, r, &r->selector_text, JS_NewString(ctx, parsed));
    free(parsed);
    return JS_UNDEFINED;
}

/* CSS Animations §6.3.2's `attribute CSSOMString name`, whose setter the specification states by NOT stating
   one: §6.3.2 gives the attribute a definition ("the name of the keyframes, used by the animation-name
   property") and no setter steps, so Web IDL's own default applies and the value is simply set. It is NOT the
   `<keyframes-name>` grammar asked again — `rule.name = 'initial'` is a name every engine accepts and
   css/cssom/CSSKeyframesRule.html reads back, where `@keyframes initial {}` is a rule §3 refuses to make. The
   two are different questions, and §6.4's serialization is what makes the accepted one round-trip: it writes
   the excluded keywords AS A STRING. */
static JSValue js_rule_set_keyframes_name(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    CssRuleData *r = rule_here_typed(ctx, this_val, RULE_TYPE_KEYFRAMES, "CSSKeyframesRule");

    (void)magic;
    if (!r) return JS_EXCEPTION;
    DCHECK(JS_IsString(val),
           "§6.3.2's `name` setter reached its body with a value that is not a string. The declaration runs "
           "the CSSOMString conversion (and parks on the page's `toString` if there is one) before the body "
           "is entered, so what arrives here is always the converted value");
    rule_set(ctx, r, &r->keyframes_name, JS_DupValue(ctx, val));
    return JS_UNDEFINED;
}

/* ---- CSS Animations §6.3.3 through §6.3.6 — the keyframes rule's own list members --------------------------- */

/* §6.3.5 and §6.3.6's MATCH, which the two sections state in identical words: "The number and order of the
   values in the specified keyframe selector must match those of the targeted keyframe rule(s). The match is
   not sensitive to white space around the values in the list." Both are therefore a comparison of CANONICAL
   forms — the stored keyText against the same grammar run over `select` — which is also what makes §6.3.6's
   worked example come out right in both directions: `findRule('to')` finds the `100%` rule because both
   canonicalise to `100%`, and `findRule('75%')` finds nothing when the rule's own selector is `25%, 75%`,
   because a keyframe selector list is a LIST and not a set of keys.
   Answers the index of the LAST match — "the last declared CSSKeyframeRule matching" — or -1, which covers
   both "no rule matches" and "`select` is not a keyframe selector list at all". */
static int keyframes_match_last(JSContext *ctx, CssRuleData *r, const char *select)
{
    char *want = css_prelude_keyframe_selectors(select, strlen(select));
    uint32_t n, i;
    int found = -1;

    if (!want) return -1;
    n = array_len(ctx, r->child_rules);
    for (i = 0; i < n; i++) {
        JSValue kid = JS_GetPropertyUint32(ctx, r->child_rules, i);
        CssRuleData *kr = rule_of(kid);
        size_t kl = 0;
        char *have;

        DCHECK(kr != NULL && kr->type == RULE_TYPE_KEYFRAME,
               "a `@keyframes` rule's child list holds something that is not a CSSKeyframeRule — §3 says the "
               "rule list inside one can only contain `<keyframe-block>` rules, and the two things that put a "
               "rule in this list (the parse and `appendRule`) both build one through keyframe_rule_new");
        have = kr ? rule_text_copy(ctx, kr->selector_text, &kl) : NULL;
        if (have && strcmp(have, want) == 0) found = (int)i;
        free(have);
        JS_FreeValue(ctx, kid);
    }
    free(want);
    return found;
}

/* §6.3.4: "The appendRule method appends the passed CSSKeyframeRule at the end of the keyframes rule. rule:
   The rule to be appended, expressed in the same syntax as one entry in the @keyframes rule. A VALID rule is
   always appended e.g. even if its key(s) already exists." — so a duplicate key is appended and never
   replaces, which is what makes §6.3.5's "the LAST declared" a question with two answers. "No Exceptions", so
   text that is not one entry is simply not appended: no SyntaxError, no IndexSizeError, nothing.
   IT PARSES THROUGH THE ONE BUILDER, with THIS rule as the enclosing rule, which is what makes the parsed
   text mean what it means inside a `@keyframes` — `0% { }` is a keyframe block here and an invalid style rule
   at a sheet's top level, and the enclosing rule is the only thing that says which. */
static JSValue js_rule_append_rule(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    CssRuleData *r = rule_here_typed(ctx, this_val, RULE_TYPE_KEYFRAMES, "CSSKeyframesRule");
    RuleBuild b;
    JSValue scratch, built;
    const char *text;
    unsigned n;

    (void)magic;
    if (!r) return JS_EXCEPTION;
    DCHECK(argc >= 1, "§6.3.4's appendRule reached its body with no rule — its IDL argument is required");
    text = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!text) return JS_EXCEPTION;
    /* A SCRATCH LIST, so that text which is not exactly one keyframe block was never in the page's list. */
    scratch = JS_NewArray(ctx);
    CHECK(!JS_IsException(scratch), "cssom: the appendRule scratch list could not be allocated");
    b.ctx = ctx;
    b.sheet = r->parent_style_sheet;
    b.top_parent = this_val;
    b.top_list = scratch;
    n = build_run(&b, text, strlen(text));
    build_free(&b);
    JS_FreeCString(ctx, text);
    DCHECK(!b.unbuilt[0],
           "a `@keyframes` body parse met an at-rule with no §6.4 interface. Inside one, §3 admits only "
           "`<keyframe-block>` rules and rule_from_parse drops every at-rule before it can reach the crash, "
           "so this is that arm having stopped being taken");
    if (n != 1) { JS_FreeValue(ctx, scratch); return JS_UNDEFINED; }
    built = JS_GetPropertyUint32(ctx, scratch, 0);
    JS_FreeValue(ctx, scratch);
    DCHECK(css_rule_is(built), "§6.3.4's appendRule built something that is not a CSS rule");
    JS_SetPropertyUint32(ctx, r->child_rules, array_len(ctx, r->child_rules), built);
    return JS_UNDEFINED;
}

/* §6.3.5: "The deleteRule method deletes the last declared CSSKeyframeRule matching the specified keyframe
   selector. If no matching rule exists, the method does nothing." No exceptions, and no §6.4 remove-a-CSS-rule
   either: that algorithm's own steps are about `@namespace` ordering in a SHEET, and this list is a
   `@keyframes` body. What it does share is the ORPHANING — a removed rule's parent CSS rule and parent CSS
   style sheet become null, which §6.4's own note calls "the only circumstance where null is returned". */
static JSValue js_rule_kf_delete_rule(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    CssRuleData *r = rule_here_typed(ctx, this_val, RULE_TYPE_KEYFRAMES, "CSSKeyframesRule");
    const char *select;
    JSValue old;
    int at;

    (void)magic;
    if (!r) return JS_EXCEPTION;
    DCHECK(argc >= 1, "§6.3.5's deleteRule reached its body with no selector — its IDL argument is required");
    select = JS_ToCString(ctx, argv[0]);
    if (!select) return JS_EXCEPTION;
    at = keyframes_match_last(ctx, r, select);
    JS_FreeCString(ctx, select);
    if (at < 0) return JS_UNDEFINED;
    old = JS_GetPropertyUint32(ctx, r->child_rules, (uint32_t)at);
    rules_remove_at(ctx, r->child_rules, (uint32_t)at);
    rule_orphan(ctx, old);
    JS_FreeValue(ctx, old);
    return JS_UNDEFINED;
}

/* §6.3.6: "The findRule returns the last declared CSSKeyframeRule matching the specified keyframe selector."
   Its IDL return type is `CSSKeyframeRule?`, so no match is NULL — §6.3.6's own example says so outright
   ("will set red to null"). */
static JSValue js_rule_find_rule(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                 int magic)
{
    CssRuleData *r = rule_here_typed(ctx, this_val, RULE_TYPE_KEYFRAMES, "CSSKeyframesRule");
    const char *select;
    int at;

    (void)magic;
    if (!r) return JS_EXCEPTION;
    DCHECK(argc >= 1, "§6.3.6's findRule reached its body with no selector — its IDL argument is required");
    select = JS_ToCString(ctx, argv[0]);
    if (!select) return JS_EXCEPTION;
    at = keyframes_match_last(ctx, r, select);
    JS_FreeCString(ctx, select);
    return at < 0 ? JS_NULL : JS_GetPropertyUint32(ctx, r->child_rules, (uint32_t)at);
}

/* §6.3.3's INDEXED PROPERTY GETTER — "returns the CSSKeyframeRule from the list of keyframes at the indicated
 * position ... The found rule or UNDEFINED if there is no rule at the specific index", which is Web IDL §3.9's
 * supported property indices exactly and is why this is core/idl_indexed.h's mechanism rather than a second
 * index parse. The `length` beside it is the attribute above, so §3.7.10 also gives the prototype
 * %Array.prototype.values% as its @@iterator.
 *
 * IT LIVES ON THE RULE CLASS AND NOT ON A COLLECTION OBJECT, which is the whole reason the mechanism had to be
 * separable from that file's own class: a CSSKeyframesRule is a CSS RULE — it carries this component's record
 * behind this component's class opaque — and it ALSO answers `rule[0]`. One object cannot be two classes, so
 * the exotic hooks are this class's and the algorithm behind them is the shared one.
 * EVERY OTHER RULE ANSWERS NOTHING HERE, and says so by having no decl: the hooks below hand back a NULL decl
 * for any type but this one, which core/idl_indexed.h reads as "not an indexed interface" and which leaves an
 * ordinary property lookup exactly as it was. */
static uint32_t keyframes_indexed_length(JSContext *ctx, JSValueConst self)
{
    CssRuleData *r = rule_of(self);

    DCHECK(r != NULL && r->type == RULE_TYPE_KEYFRAMES,
           "§6.3.3's indexed getter was asked for its length by something that is not a CSSKeyframesRule — the "
           "decl is handed out only for one type, so reaching this from another is that test having changed");
    return r ? array_len(ctx, r->child_rules) : 0;
}

static JSValue keyframes_indexed_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    CssRuleData *r = rule_of(self);
    JSValue kid;

    DCHECK(r != NULL && r->type == RULE_TYPE_KEYFRAMES,
           "§6.3.3's indexed getter was asked for an item by something that is not a CSSKeyframesRule");
    if (!r) return JS_UNDEFINED;
    kid = JS_GetPropertyUint32(ctx, r->child_rules, i);
    DCHECK(JS_IsUndefined(kid) || css_rule_is(kid),
           "a `@keyframes` rule's child list holds something that is not a CSS rule — its indexed getter "
           "declares `CSSKeyframeRule`, and the parse and `appendRule` are the only things that put one in");
    return kid;
}

static const IdlIndexedDecl KEYFRAMES_INDEXED = { "CSSKeyframesRule", keyframes_indexed_length,
                                                  keyframes_indexed_item, NULL, 0 };

/* The decl for THIS object, or NULL — the one place the "is this the interface with the indexed getter"
   question is asked, so the two hooks below cannot answer it differently.
   THROUGH JS_GetOpaque AND NOT THE ACCESSOR, which is the one place in this file that is right. The hooks run
   on every own-property MISS on every rule object, and a miss is not a reach: capturing there would put a
   record into the running flow's delta because a page read `styleRule.foo`. The two callbacks above do use the
   accessor, because they run only for a real index on a real `@keyframes`, which IS a reach. */
static const IdlIndexedDecl *rule_indexed_decl(JSValueConst obj)
{
    CssRuleData *r = JS_GetOpaque(obj, g_rule_class);

    return (r && r->type == RULE_TYPE_KEYFRAMES) ? &KEYFRAMES_INDEXED : NULL;
}

static int rule_get_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    return idl_indexed_own_property(ctx, desc, obj, prop, rule_indexed_decl(obj));
}

static int rule_own_property_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    return idl_indexed_own_property_names(ctx, ptab, plen, obj, rule_indexed_decl(obj));
}

static JSClassExoticMethods g_rule_exotic = {
    .get_own_property = rule_get_own_property,
    .get_own_property_names = rule_own_property_names,
    /* An index parse and a read of this component's own Array. A Web IDL indexed property getter has no
       accessor by construction, which is what lets the engine's own read path run it from C. */
    .get_own_property_no_user_code = true,
};

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

/* THE FOUR `style` ATTRIBUTES, WHICH ARE ONE MEMBER OVER ONE FIELD. §6.4.3 states it: "the style attribute
   must return a CSSStyleProperties object for the style rule, with the following properties: computed flag
   unset, readonly flag unset, declarations THE DECLARED DECLARATIONS IN THE RULE, parent CSS rule THIS, owner
   node null" — and CSS Fonts 5 §9.1's, §6.4.7's and §6.4.8's say the same five things. [SameObject], so the
   block is remembered on the record: a page holds `rule.style` and compares it, and a fresh object per read
   makes every such comparison false. A rule has exactly ONE block object, which is why they share `r->style`.
   WHAT DIFFERS IS THE INTERFACE, and the table says so once: §6.4.3's and §6.4.8's hand back a
   CSSStyleProperties, CSS Fonts 5 §9.1's a CSSFontFaceDescriptors, §6.4.7's a CSSPageDescriptors. §6.4.8's
   choice is the corpus's own — css/cssom/idlharness.html lists `sheet.cssRules[2].cssRules[0].style` under
   CSSStyleProperties, where it lists `sheet.cssRules[2].style` under CSSPageDescriptors. The IDL in
   @webref/idl types it `CSSStyleDeclaration`, which a CSSStyleProperties IS; the editor's draft types it
   `CSSMarginDescriptors`, an interface CSSOM references twice and never declares anywhere, so there is no
   member list to build one from and the corpus contradicts it outright.
   CSS Animations §6.2.2's is the fifth and it says the same five things about a `<keyframe-block>` ("readonly
   flag unset, declarations the declared declarations in the rule in specified order, parent CSS rule the
   context object, owner node null"), and its IDL types it `CSSStyleProperties` — the editor's draft and the
   @webref/idl corpus agree on that word, so there is no third interface here and no disagreement to settle.
   IT IS MINTED THROUGH THE CAPTURING ACCESSOR, which is what makes the memo per-flow: the block belongs to the
   flow that first asked for it, and a sibling that asks gets its own over the same declarations. */
enum { STYLE_OF_STYLE_RULE = 0, STYLE_OF_FONT_FACE, STYLE_OF_PAGE, STYLE_OF_MARGIN, STYLE_OF_KEYFRAME };

static JSValue js_rule_style(JSContext *ctx, JSValueConst this_val, int magic)
{
    static const struct {
        uint16_t    type;
        const char *iface;
        JSValue   (*create)(JSContext *, JSValueConst);
    } OF[] = {
        { RULE_TYPE_STYLE,     "CSSStyleRule",    cssom_style_properties_for_rule },
        { RULE_TYPE_FONT_FACE, "CSSFontFaceRule", cssom_font_face_descriptors_for_rule },
        { RULE_TYPE_PAGE,      "CSSPageRule",     cssom_page_descriptors_for_rule },
        { RULE_TYPE_MARGIN,    "CSSMarginRule",   cssom_style_properties_for_rule },
        { RULE_TYPE_KEYFRAME,  "CSSKeyframeRule", cssom_style_properties_for_rule },
    };
    CssRuleData *r;

    DCHECK(magic >= 0 && magic < (int)(sizeof(OF) / sizeof(OF[0])),
           "a `style` attribute ran with a magic no interface in this table declares — the magic is written by "
           "the prototype install below and by nothing else");
    r = rule_here_typed(ctx, this_val, OF[magic].type, OF[magic].iface);
    if (!r) return JS_EXCEPTION;
    if (!JS_IsObject(r->style)) {
        rule_set(ctx, r, &r->style, OF[magic].create(ctx, this_val));
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
    DCHECK(r->type == RULE_TYPE_STYLE || r->type == RULE_TYPE_FONT_FACE || r->type == RULE_TYPE_PAGE ||
           r->type == RULE_TYPE_MARGIN || r->type == RULE_TYPE_KEYFRAME,
           "§6.6's declaration block wrote its text back onto a rule that HAS no declaration block. A rule's "
           "`style` attribute is the only thing that reaches this, and it is declared by §6.4.3's "
           "CSSStyleRule, CSS Fonts 5 §9.1's CSSFontFaceRule, §6.4.7's CSSPageRule, §6.4.8's CSSMarginRule and "
           "CSS Animations §6.2's CSSKeyframeRule and by nothing else in this build");
    /* THE RULE'S OWN RESTRICTION, ON THE WRITE SIDE. `setProperty`, `style.cssText =` and every descriptor
       attribute reach the record through this one function, so filtering here is what makes
       `pageRule.style.setProperty('transform', 'scale(1)')` and
       `keyframeRule.style.setProperty('animation-name', 'none')` take no effect and leaves `length` at what
       the context really declares — css/cssom/rule-restrictions.html reads both, on both rules, through both
       `setProperty` and `cssText`. It is the same call the three restricted creators make, so the text a
       write leaves behind is the text a parse would have. */
    if (rule_block_context(r->type) != CSSOM_BLOCK_UNRESTRICTED) {
        char *kept = cssom_serialize_declarations(text, len, rule_block_context(r->type));

        rule_set(ctx, r, &r->block_text, JS_NewString(ctx, kept ? kept : ""));
        free(kept);
        return;
    }
    rule_set(ctx, r, &r->block_text, JS_NewStringLen(ctx, text, len));
}

/* ---- the AUTHOR CASCADE's view ----------------------------------------------------------------------------- */

/* The walk's own state: the text it is building, the per-emitted-rule layer it is building beside it, and the
   §6.4.3 order it declares every layer it meets into. See css_rule.h for why the second and third exist. */
typedef struct {
    RBuf                 out;
    const CssLayerNode **layer;
    uint32_t             n, cap;
    CssLayerOrder       *order;
} CascadeEmit;

/* Record which cascade layer the rule just written into `e->out` belongs to. NULL is §6.4's non-style rules —
   see the header for why the caller reads that as a statement rather than as a hole. */
static void cascade_emit_mark(CascadeEmit *e, const CssLayerNode *layer)
{
    if (e->n == e->cap) {
        uint32_t cap = e->cap ? e->cap * 2 : 16;
        const CssLayerNode **grown = realloc(e->layer, (size_t)cap * sizeof(*grown));

        CHECK(grown != NULL, "cssom: OOM recording a rule's cascade layer for the author cascade");
        e->layer = grown;
        e->cap = cap;
    }
    e->layer[e->n++] = layer;
}

/* WHAT `&` NAMES WHERE THE WALK CURRENTLY IS — CSS Nesting §4's "the elements matched by the parent rule", as
   the nearest ancestor style rule's selector list IN THE FORM THE WALK EMITTED IT. `sel` is NULL at a sheet's
   top level, which is the positive statement that no rule here is nested and none of them resolves anything.
   IT CARRIES THE EMITTED FORM AND NOT THE STORED ONE, which is what makes arbitrary depth fall out of one
   level's rule: an inner rule of `.a { .b { .c { } } }` resolves against `:is(.a) .b`, so it comes out as
   `:is(:is(.a) .b) .c` — whose specificity, by Selectors 4 §15 "Calculating a selector's specificity", is the
   (0,3,0) the flattened `.a .b .c` has. Resolving against the STORED `& .b` would leave an unresolved nesting
   selector in the sheet the matcher parses. */
typedef struct { const char *sel; size_t len; } CascadeNest;

static bool cascade_emit(JSContext *ctx, JSValueConst list, CascadeEmit *e, CssLayerNode *cur,
                         const CascadeNest *nest);

/* One rule's contribution to the text the selector matcher re-parses, and to §6.4.3's layer order. A STYLE rule
   contributes itself, in the layer it is nested in, FOLLOWED BY ITS OWN NESTED RULES; a CONDITIONAL GROUP rule
   contributes its children when its condition holds and nothing when it does not, which is what `@media` MEANS
   and is the whole reason the cascade cannot simply read a sheet's top level. `cur` is the cascade layer this
   rule sits in — §6.4.3's implicit outer layer at a sheet's top level, and the layer a `@layer` block declares
   inside it. `nest` is what a nested rule's `&` resolves to; see above. */
static bool cascade_emit_one(JSContext *ctx, JSValueConst rule, CascadeEmit *e, CssLayerNode *cur,
                             const CascadeNest *nest)
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
        /* §6.4.3: "Layers that are defined inside of a conditional group rule do not contribute to the layer
           order unless the condition is true or unless the conditional group rule can evaluate differently for
           different elements in the document." A `@media` is global to the document, so a false condition is
           the first arm and the children — layers included — are simply not walked. The second arm is the
           element-sensitive conditional, which `@container` below is and which is why that rule cannot reach
           this shape however much its IDL resembles this one's. */
        if (!applies) return true;
        kids = rule_child_rules(ctx, rule);
        ok = cascade_emit(ctx, kids, e, cur, nest);
        JS_FreeValue(ctx, kids);
        return ok;
    }
    /* CSS Conditional §6's `@supports`, the OTHER conditional group rule, and §2's sentence covers both
       identically: "when the condition is true, CSS processors must apply the rules inside the group rule as
       though they were at the group rule's location; when the condition is false, CSS processors must not
       apply any of rules inside the group rule."
       §6.4.3's layer sentence lands on the first arm here too — a feature query is global to the document, so
       a false condition means the children, layers included, are simply not walked. It is not the
       element-sensitive conditional that sentence's second arm is about. */
    if (r->type == RULE_TYPE_SUPPORTS) {
        JSValue kids;
        bool ok;

        if (!rule_supports_matches(ctx, r)) return true;
        kids = rule_child_rules(ctx, rule);
        ok = cascade_emit(ctx, kids, e, cur, nest);
        JS_FreeValue(ctx, kids);
        return ok;
    }
    /* CSS Conditional 5 §5.4's `@container` IS THE ELEMENT-SENSITIVE CONDITIONAL, and that is what makes it a
       different problem from the two arms above rather than a third copy of them. A media query and a feature
       query are facts about the DOCUMENT, so each is asked once and its children are walked or not. §5.4's
       condition is asked PER ELEMENT: "for each element, the query container to be queried is selected from
       among the element's ancestor query containers", and two elements matching one selector inside one
       `@container` can therefore get opposite answers.
       THERE IS NO ARM OF THIS WALK THAT IS RIGHT, WHICH IS WHY THIS CRASHES INSTEAD OF PICKING ONE. This
       cascade flattens the rules that apply into TEXT that is re-parsed and matched by selector, so emitting
       the children applies them to every element the selector matches — the query is then true for everybody —
       and not emitting them applies them to nobody, which is the query being false for everybody. Both are a
       plausible answer that reads exactly like a right one, and §5.4 says the third possibility is the common
       case rather than an edge: "if no ancestor is an eligible query container, then the container query is
       UNKNOWN for that element."
       AND SKIPPING IT IS WRONG EVEN FOR THE LAYERS. §6.4.3's sentence one arm up has a second clause written
       for exactly this rule — layers inside a conditional group rule contribute "unless the conditional group
       rule can evaluate differently for different elements in the document" — so a `@container`'s layers
       contribute to the layer order UNCONDITIONALLY, which the `@media` arm's false branch does not do and
       must not be reused for.
       WHAT TO BUILD, IN ORDER: §5.1 "Creating Query Containers: the container-type property" resolved on
       ancestors (so the query container can be SELECTED, which is a cascade result feeding a cascade input and
       is the part that has to be designed rather than added), then §6.1 "Size Container Features" against that
       container's principal box — which needs core/browser/layout — and §6.2 "Style Container Features", which
       needs only the computed value of a custom property on the container and is therefore the arm that can
       land first. §5.4's three-valued outcome is MQ4's and not a boolean: `<general-enclosed>` and an
       unselectable container are both UNKNOWN, which does not match. */
    if (r->type == RULE_TYPE_CONTAINER) {
        DFAIL("CSS Conditional 5 §5.4 \"Container Queries: the @container rule\" reached the author cascade, "
              "and this build cannot decide it: a container query is evaluated PER ELEMENT against a query "
              "container selected from that element's ancestors (§5.4), while this walk flattens the rules "
              "that apply into one text matched by selector, so it has nowhere to put a condition that is true "
              "for one matching element and unknown for its sibling. Do NOT resolve it to a boolean here — "
              "emitting the children makes the query true for every element and dropping them makes it false "
              "for every element, and §5.4's own answer for a document with no eligible container is UNKNOWN, "
              "which is neither. Build §5.1's `container-type` resolution so a query container can be "
              "selected, then §6.2's style container features (which need only a computed custom property on "
              "the container) and §6.1's size features (which need core/browser/layout); and note that "
              "§6.4.3's layer sentence puts THIS rule in its second arm — a `@container`'s layers contribute "
              "to the layer order whatever its condition says, so the `@media` arm's skip is not the shape to "
              "copy. The CSSOM object is complete and is NOT what is missing: core/css/css_rule.c builds a "
              "CSSContainerRule with §9.1's `conditions`, `containerName`, `containerQuery` and "
              "`conditionText`, so the rule, its children and its `cssText` are all readable by the page and "
              "only its CASCADED EFFECT is unbuilt");
    }
    /* AN `@namespace` IS EMITTED, and it is the one non-style rule that must be: it declares a prefix the
       SELECTORS below are written against, so a sheet whose `@namespace svg url(...)` were dropped would hand
       lexbor `svg|a { … }` with no `svg` bound — an invalid selector, which the parse then drops, which
       silently un-styles every element the page selected that way. It goes in verbatim (the serialization is
       the canonical form, terminated by its own `;`) and it TAKES A SLOT in the layer list, because that list
       is indexed by the re-parse's rule position and this rule occupies one — its entry is NULL, which is what
       tells the caller the rule at that index matches no element rather than that its layer went missing. */
    if (r->type == RULE_TYPE_NAMESPACE) {
        RBuf one = { NULL, 0, 0 };

        if (!namespace_rule_serialize(ctx, r, &one)) { free(one.s); return false; }
        rbuf_add(&e->out, one.s);
        free(one.s);
        cascade_emit_mark(e, NULL);
        return true;
    }
    /* AN `@import` CONTRIBUTES NO STYLE RULE TO THIS SHEET AND STILL CONTRIBUTES TO THE LAYER ORDER. Its
       declarations belong to the IMPORTED sheet — CSS Cascade §2 treats its contents "as if they were written
       in place of the @import rule" — and this build fetches no imported sheet, which css_rule.h records as the
       gap that `styleSheet` is absent for. But §6.4.1 lists it first among the three ways a cascade layer is
       DECLARED ("using an @import rule with the layer keyword or layer() function, assigning the contents of
       the imported file into that layer"), and §6.4.3 orders layers by where they are first declared — so
       `@import url(a) layer(theme); @layer other { } @layer theme { }` puts `theme` FIRST, and an import whose
       layer went unrecorded would order those two backwards for every rule in them. The sheet being unfetched
       does not change where its layer sits. */
    if (r->type == RULE_TYPE_IMPORT) {
        char *ln = rule_opt_text(ctx, r->layer_name);
        CssLayerNames names = { NULL, 0 };
        bool named;

        /* §6.4.4's `layerName` — NULL for an import that declares no layer, the EMPTY STRING for the anonymous
           `layer` keyword (§6.4.2.1: "an @import rule uses the layer keyword (which does not provide a
           <layer-name>) ... its layer name gains a unique anonymous segment"), and the name otherwise. */
        if (!ln) return true;
        named = css_prelude_layer_names(ln, strlen(ln), &names);
        DCHECK(named,
               "an `@import` rule's stored `layerName` is not a `<layer-name>`. CSS Cascade §2's grammar is "
               "`[ layer | layer(<layer-name>) ]?` and a name outside that production makes the whole at-rule "
               "invalid, so the refusal belongs in the import prelude's own parse — core/css/"
               "css_at_rule_prelude.c takes the `layer()` function's RAW CONTENTS without putting them through "
               "the `<layer-name>` grammar the two `@layer` at-rules share");
        DCHECK(!named || names.n <= 1,
               "an `@import` rule declares more than one cascade layer — §2's grammar admits `layer(...)` once "
               "and its contents are a single `<layer-name>`, with no `#` multiplier on it");
        if (named) css_layer_order_declare(e->order, cur, names.n ? names.v[0] : NULL);
        css_layer_names_free(&names);
        free(ln);
        return true;
    }
    /* AN `@font-face` DECLARES A FONT FACE AND NOT A STYLE: nothing it contains can match an element, so it is
       not a rule the selector matcher has anything to do with, and it declares no layer of its own — §6.4's own
       note that at-rules "defined inside cascade layers also use the layer order" is about the layer they are
       IN, which `cur` already is. */
    if (r->type == RULE_TYPE_FONT_FACE) return true;
    /* AND NEITHER DOES AN `@page`, for a third reason of its own: its declarations style the PAGE BOX, which
       CSS Paged Media §3 makes a box outside the document tree. Its page selector list selects pages and not
       elements — `named:first` matches no element, and there is no element it could — so nothing in it can
       reach the selector matcher. §4.3's margin at-rules are inside it and go with it. */
    if (r->type == RULE_TYPE_PAGE) return true;
    /* NOR DOES A `@keyframes`, and for a FOURTH reason: CSS Animations §3 says its rule list "can only contain
       <keyframe-block> rules", and a keyframe block's prelude is a `<keyframe-selector>#` — a position along a
       duration, which selects no element and cannot. Its declarations reach an element only through the
       ANIMATION that names it (§4.1's `animation-name`), which is a step after the cascade rather than a rule
       in it — CSS Cascade §6.1 puts animations in an origin of their own, above every author declaration. */
    if (r->type == RULE_TYPE_KEYFRAMES) return true;
    /* NOR DOES AN `@property`, and for a FIFTH: CSS Properties and Values API 1 §3 makes it "a custom property
       REGISTRATION directly in a stylesheet", whose body declares §3.1's, §3.2's and §3.3's descriptors and no
       property of any element. It has no selector, so it matches nothing and cannot; what it changes is how a
       custom property's value is PARSED at computed-value time (§2.2 through §2.4), which is a step below the
       cascade and reads the registration rather than this rule list. */
    if (r->type == RULE_TYPE_PROPERTY) return true;
    /* A §6.4.4.2 `@layer` STATEMENT CONTRIBUTES ONLY TO THE ORDER, which is the whole of what the at-rule is
       for: it is "declaring a named layer WITHOUT ASSIGNING ANY RULES" (CSS Cascade §6.4.1), so there is
       nothing inside it for a selector to match and nothing to emit. What it decides is where those layers sit
       — §6.4.4.2's own note says so ("since layer ordering is defined by first occurrence of the layer name,
       this rule allows a page to declare the order of its layers up front, so that their order is apparent
       without having to read the entire style sheet") — and its names are declared LEFT TO RIGHT because
       §6.4.4.2 says they are: "multiple comma-separated layer names can be provided in this syntax, declaring
       each of the layers IN THE ORDER SPECIFIED." */
    if (r->type == RULE_TYPE_LAYER_STATEMENT) {
        char **names;
        unsigned n, i;

        if (!rule_layer_names(ctx, r, &names, &n)) return false;
        DCHECK(n >= 1, "a §8.2 layer statement rule declares NO `<layer-name>` — §6.4.4.2's `#` multiplier has "
                       "no zero-length arm, and its creator refuses a prelude with no name in it");
        for (i = 0; i < n; i++) css_layer_order_declare(e->order, cur, names[i]);
        serialized_free(names, n);
        return true;
    }
    /* A §6.4.4.1 `@layer` BLOCK DECLARES A LAYER AND THEN CONTRIBUTES ITS CHILDREN INTO IT. §6.4.4.1 makes the
       second half exactly the `@media` arm above — "such @layer block rules have the same restrictions and
       processing as a conditional group rule [CSS-CONDITIONAL-3] with a TRUE condition" — so the only thing
       this arm adds to that one is the layer the children are walked under. A rule that declares NO name is
       §6.4.2.1's anonymous layer and gets a node of its own every time, which is why the name is handed on as
       NULL rather than as an empty string: "each occurrence of an anonymous layer declaration represents a
       unique cascade layer". */
    if (r->type == RULE_TYPE_LAYER_BLOCK) {
        CssLayerNode *node;
        char **names;
        unsigned n;
        JSValue kids;
        bool ok;

        if (!rule_layer_names(ctx, r, &names, &n)) return false;
        DCHECK(n <= 1, "a §8.1 layer block rule declares more than one `<layer-name>` — §6.4.4.1's grammar is "
                       "`<layer-name>?`, and its creator refuses a prelude carrying a list");
        node = css_layer_order_declare(e->order, cur, n ? names[0] : NULL);
        serialized_free(names, n);
        kids = rule_child_rules(ctx, rule);
        ok = cascade_emit(ctx, kids, e, node, nest);
        JS_FreeValue(ctx, kids);
        return ok;
    }
    DCHECK(r->type == RULE_TYPE_STYLE, "the author cascade met a rule type it has no arm for");
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
    /* CSS NESTING IS RESOLVED, NEVER FLATTENED. A nested rule's stored selector is CSS Nesting §6 "CSSOM"'s
       absolutized `<relative-selector-list>`, so it always names its parent with a nesting selector; §4
       "Nesting Selector: the & selector" desugars that "by replacing it with the parent style rule's selector,
       wrapped in an :is() selector". Concatenating the parent's text instead would match the same elements and
       CASCADE DIFFERENTLY — §4's own worked example is exactly that — because Selectors 4 §15 "Calculating a
       selector's specificity" gives `:is()` the specificity of its most specific argument rather than of the
       one that matched.
       THE RESOLVED TEXT IS PARSED BEFORE IT IS EMITTED, and what goes into the sheet is what that parse
       serialized. Two things ride on it. §3.1 "Syntax": "An invalid nested style rule is ignored, along with
       its contents, but does not invalidate its parent rule" — a rule whose resolved selector is not a selector
       list contributes nothing and takes its children with it, which is the `return true` below. And the
       emission's per-index round trip: core/css/css_style_declaration.c reads each emitted rule's cascade layer
       BY POSITION in the re-parse, so a rule that came back as a different kind of rule would shift every rule
       after it into a neighbour's layer. Emitting the parse's own serialization makes that impossible rather
       than merely unlikely. */
    if (nest) {
        char *desugared = css_nesting_resolve(sel, sl, nest->sel, nest->len);
        char *canonical = selector_list_reserialize(desugared, strlen(desugared));

        free(desugared);
        free(sel);
        if (!canonical) return true;
        sel = canonical;
        sl = strlen(canonical);
    }
    block = rule_text_copy(ctx, r->block_text, &bl);
    rbuf_add_n(&e->out, sel, sl);
    rbuf_add(&e->out, "{");
    if (block) rbuf_add_n(&e->out, block, bl);
    rbuf_add(&e->out, "}");
    free(block);
    cascade_emit_mark(e, cur);
    /* AND THEN ITS OWN NESTED RULES, AFTER IT, which §3.4 "Mixing Nesting Rules and Declarations" requires
       rather than merely permits: "For the purpose of determining the Order Of Appearance, nested style rules
       and nested group rules are considered to come after their parent rule." So the parent's declarations go
       in first and the children follow in document order, and CSS Cascade §6.1's Order of Appearance — which
       the emission's own position IS — comes out right for `article { color: blue; & { color: red } }`.
       They resolve against THIS rule's emitted selector, and the recursion is the rule tree's own depth: CSS
       Nesting places no limit on it and neither does this walk. */
    if (array_len(ctx, r->child_rules) == 0) { free(sel); return true; }
    {
        JSValue kids = rule_child_rules(ctx, rule);
        CascadeNest inner = { sel, sl };
        bool ok = cascade_emit(ctx, kids, e, cur, &inner);

        JS_FreeValue(ctx, kids);
        free(sel);
        return ok;
    }
}

static bool cascade_emit(JSContext *ctx, JSValueConst list, CascadeEmit *e, CssLayerNode *cur,
                         const CascadeNest *nest)
{
    uint32_t n = array_len(ctx, list), i;

    DCHECK(!nest || (nest->sel != NULL && nest->len > 0),
           "the author cascade descended into a nested rule list with an EMPTY parent selector to resolve `&` "
           "against. §4's nesting selector is \"the elements matched by the parent rule\", and the style arm "
           "above emits nothing at all for a rule whose selector list it could not read");
    for (i = 0; i < n; i++) {
        JSValue rule = JS_GetPropertyUint32(ctx, list, i);
        bool ok = cascade_emit_one(ctx, rule, e, cur, nest);

        JS_FreeValue(ctx, rule);
        if (!ok) return false;
    }
    return true;
}

void css_rule_cascade_sheet_free(CssRuleCascadeSheet *s)
{
    DCHECK(s != NULL, "the author cascade's view of a sheet was freed through nothing");
    free(s->text);
    free((void *)s->layer);
    s->text = NULL;
    s->layer = NULL;
    s->n = 0;
}

bool css_rule_cascade_sheet(JSContext *ctx, JSValueConst list, CssLayerOrder *order,
                            CssRuleCascadeSheet *out)
{
    CascadeEmit e = { { NULL, 0, 0 }, NULL, 0, 0, NULL };

    DCHECK(out != NULL, "the author cascade's view of a sheet was built with nowhere to report it");
    DCHECK(order != NULL,
           "a sheet was flattened for the author cascade with no §6.4.3 LAYER ORDER to declare its layers "
           "into. Every `@layer` rule the walk meets declares one, and the order is what §6.1's Layers "
           "criterion sorts by — a walk with nowhere to put them would answer every layer's rules as if they "
           "were unlayered, which inverts the cascade for the whole sheet");
    e.order = order;
    if (!cascade_emit(ctx, list, &e, css_layer_order_root(order), NULL)) {
        free(e.out.s);
        free(e.layer);
        return false;
    }
    out->text = e.out.s;
    out->layer = e.layer;
    out->n = e.n;
    DCHECK(out->n == 0 || out->text != NULL,
           "the author cascade emitted rules and no text to hold them — the two are written by one call per "
           "rule and cannot disagree about whether a rule went in");
    return true;
}

/* ---- the interfaces -------------------------------------------------------------------------------------- */

/* §6.4.2's historical constants, which the IDL declares on the interface AND its prototype. */
static const struct { const char *name; uint32_t v; } CR_CONSTS[] = {
    { "STYLE_RULE", 1 }, { "CHARSET_RULE", 2 }, { "IMPORT_RULE", 3 }, { "MEDIA_RULE", 4 },
    { "FONT_FACE_RULE", 5 }, { "PAGE_RULE", 6 },
    /* CSS Animations §6.1.1's `partial interface CSSRule` — two more additions to a list CSSOM calls frozen,
       and §6.4.2's own `type` table lists both numbers, so the two standards agree about them outright. */
    { "KEYFRAMES_RULE", 7 }, { "KEYFRAME_RULE", 8 },
    { "MARGIN_RULE", 9 }, { "NAMESPACE_RULE", 10 },
    /* CSS Counter Styles 3 §9.1 "Extensions to the CSSRule interface" — its `partial interface CSSRule`, the
       same shape the two above have. THE NUMBER IS DECLARED WHETHER OR NOT THE RULE IS BUILT, and that is
       §6.4.2's own arrangement rather than a shortcut: the constants are a HISTORICAL enumeration a page reads
       off `CSSRule`, so `CSSRule.COUNTER_STYLE_RULE` is 11 in a document containing no `@counter-style` at all,
       exactly as `CHARSET_RULE` is 2 with no CSSCharsetRule interface anywhere in the platform and
       `SUPPORTS_RULE` is 12 here. What is NOT declared is a rule OBJECT for it — meeting `@counter-style` still
       reaches rule_unbuilt_fail, which names CSSCounterStyleRule as the thing to build. */
    { "COUNTER_STYLE_RULE", 11 },
    /* CSS Conditional §7.1's `partial interface CSSRule` — another addition to that same list, and it is here
       because that standard puts it there rather than because a number was needed. */
    { "SUPPORTS_RULE", 12 },
    /* CSS Fonts 4 §12.2 "The CSSFontFeatureValuesRule interface" — its `partial interface CSSRule`. 13 is
       skipped by the platform and not by this table: it was CSS Device Adaptation's VIEWPORT_RULE, whose
       specification was abandoned, so no standard declares that number and inventing it would be a member no
       browser has. */
    { "FONT_FEATURE_VALUES_RULE", 14 },
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
    /* THE EXOTIC IS CSS Animations §6.3.3's INDEXED PROPERTY GETTER, and it is on the class every rule shares
       because a CSSKeyframesRule is a rule. It answers nothing for every other type — see rule_indexed_decl —
       so what a CSSStyleRule gains is one NULL-decl call per own-property miss and no behaviour at all. */
    JSClassDef d = { "CSSRule", rule_finalizer, rule_gc_mark, NULL, &g_rule_exotic };

    if (g_rule_class) return;   /* one AGENT, one class and one set of pool entries */
    JS_NewClassID(JS_GetRuntime(ctx), &g_rule_class);
    JS_NewClass(JS_GetRuntime(ctx), g_rule_class, &d);
    g_proto_slot[PROTO_RULE] = realm_value_declare(ctx, "CSSOM §6.4.2 CSSRule.prototype");
    g_proto_slot[PROTO_GROUPING] = realm_value_declare(ctx, "CSSOM §6.4.5 CSSGroupingRule.prototype");
    g_proto_slot[PROTO_STYLE] = realm_value_declare(ctx, "CSSOM §6.4.3 CSSStyleRule.prototype");
    g_proto_slot[PROTO_CONDITION] = realm_value_declare(ctx, "CSS Conditional §7.2 CSSConditionRule.prototype");
    g_proto_slot[PROTO_MEDIA] = realm_value_declare(ctx, "CSS Conditional §7.3 CSSMediaRule.prototype");
    g_proto_slot[PROTO_SUPPORTS] = realm_value_declare(ctx, "CSS Conditional §7.4 CSSSupportsRule.prototype");
    g_proto_slot[PROTO_CONTAINER] =
        realm_value_declare(ctx, "CSS Conditional 5 §9.1 CSSContainerRule.prototype");
    g_proto_slot[PROTO_IMPORT] = realm_value_declare(ctx, "CSSOM §6.4.4 CSSImportRule.prototype");
    g_proto_slot[PROTO_NAMESPACE] = realm_value_declare(ctx, "CSSOM §6.4.9 CSSNamespaceRule.prototype");
    g_proto_slot[PROTO_FONT_FACE] = realm_value_declare(ctx, "CSS Fonts 5 §9.1 CSSFontFaceRule.prototype");
    g_proto_slot[PROTO_PAGE] = realm_value_declare(ctx, "CSSOM §6.4.7 CSSPageRule.prototype");
    g_proto_slot[PROTO_MARGIN] = realm_value_declare(ctx, "CSSOM §6.4.8 CSSMarginRule.prototype");
    g_proto_slot[PROTO_KEYFRAMES] = realm_value_declare(ctx, "CSS Animations §6.3 CSSKeyframesRule.prototype");
    g_proto_slot[PROTO_KEYFRAME] = realm_value_declare(ctx, "CSS Animations §6.2 CSSKeyframeRule.prototype");
    g_proto_slot[PROTO_LAYER_BLOCK] =
        realm_value_declare(ctx, "CSS Cascade §8.1 CSSLayerBlockRule.prototype");
    g_proto_slot[PROTO_LAYER_STATEMENT] =
        realm_value_declare(ctx, "CSS Cascade §8.2 CSSLayerStatementRule.prototype");
    g_proto_slot[PROTO_PROPERTY] =
        realm_value_declare(ctx, "CSS Properties and Values API 1 §6.1 CSSPropertyRule.prototype");
    g_id_set_selector = idl_setter_id(ctx, IDL_DOMSTRING, false, js_rule_set_selector, 0);
    g_id_set_page_selector = idl_setter_id(ctx, IDL_DOMSTRING, false, js_rule_set_page_selector, 0);
    g_id_set_key_text = idl_setter_id(ctx, IDL_DOMSTRING, false, js_rule_set_key_text, 0);
    g_id_set_keyframes_name = idl_setter_id(ctx, IDL_DOMSTRING, false, js_rule_set_keyframes_name, 0);
    g_id_set_css_text = idl_setter_id(ctx, IDL_DOMSTRING, false, js_rule_set_css_text, 0);
    {
        /* §6.4.5: `unsigned long insertRule(CSSOMString rule, optional unsigned long index = 0)` and
           `undefined deleteRule(unsigned long index)` — the same two shapes §6.1.2 declares, because they are
           the same two algorithms. */
        static const IdlArgType INSERT[2] = { IDL_DOMSTRING, IDL_UNSIGNED_LONG };
        static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };
        /* CSS Animations §6.3.1's three, every one of them `(CSSOMString)`. §6.3.5's `deleteRule` takes a
           keyframe SELECTOR where §6.4.5's takes an index, which is the whole reason a `@keyframes` must not
           be a §6.4.5 grouping rule — two members of one name whose arguments are different types. */
        static const IdlArgType ONE_STRING[1] = { IDL_DOMSTRING };

        g_id_insert_rule = idl_method_id(ctx, INSERT, 2, js_rule_insert_rule, 0);
        idl_optional_from(1);
        g_id_delete_rule = idl_method_id(ctx, ONE_ULONG, 1, js_rule_delete_rule, 0);
        g_id_append_rule = idl_method_id(ctx, ONE_STRING, 1, js_rule_append_rule, 0);
        g_id_kf_delete_rule = idl_method_id(ctx, ONE_STRING, 1, js_rule_kf_delete_rule, 0);
        g_id_find_rule = idl_method_id(ctx, ONE_STRING, 1, js_rule_find_rule, 0);
    }
    realm_declare_intrinsic(css_rule_install_proto);
}

void css_rule_install_proto(JSContext *ctx)
{
    JSValue base, grouping, style, condition, media, supports, container, import_rule, ns, font_face, page,
            margin;
    JSValue keyframes, keyframe, layer_block, layer_statement, property_rule;

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
    idl_install_method(ctx, grouping, "insertRule", g_id_insert_rule);
    idl_install_method(ctx, grouping, "deleteRule", g_id_delete_rule);

    style = JS_NewObjectProto(ctx, grouping);
    CHECK(!JS_IsException(style), "CSSStyleRule.prototype could not be allocated");
    idl_interface_tag(ctx, style, "CSSStyleRule");
    idl_install_accessor(ctx, style, "selectorText", js_rule_get, CR_SELECTOR_TEXT, g_id_set_selector);
    idl_install_accessor(ctx, style, "style", js_rule_style, STYLE_OF_STYLE_RULE,
                         cssom_put_forwards_setter());

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

    /* CSS Conditional §7.4's CSSSupportsRule.prototype — `interface CSSSupportsRule : CSSConditionRule`, so
       it chains off `condition` beside CSSMediaRule and NOT off it. `matches` is the ONE member §7.4 declares;
       `conditionText` is §7.2's, inherited from the prototype above and redefined for this interface by the
       spec's own text rather than by a second accessor here (installing one would put two definitions of one
       attribute on one chain, which is exactly what §7.2's "derived interfaces may specify different
       behavior" does NOT mean). */
    supports = JS_NewObjectProto(ctx, condition);
    CHECK(!JS_IsException(supports), "CSSSupportsRule.prototype could not be allocated");
    idl_interface_tag(ctx, supports, "CSSSupportsRule");
    idl_install_accessor(ctx, supports, "matches", js_rule_get, CR_SUPPORTS_MATCHES, -1);

    /* CSS Conditional 5 §9.1's CSSContainerRule.prototype — `interface CSSContainerRule : CSSConditionRule`,
       so it chains off `condition` beside CSSMediaRule and CSSSupportsRule and NOT off either of them: the
       three conditional at-rules are siblings. `conditionText` is §7.2's, inherited from the prototype above
       and redefined for this interface by §9.1's own algorithm rather than by a second accessor here — the
       same arrangement CSSSupportsRule has, and for the same reason (two definitions of one attribute on one
       chain is not what §7.2's "derived interfaces may specify different behavior" means).
       THERE IS NO `matches` HERE, and its absence is §9.1's rather than a gap: CSSMediaRule and
       CSSSupportsRule each declare one because their condition is a fact about the DOCUMENT, and a container
       query is a fact about an ELEMENT's query container — there is no receiver on the rule for it to be
       asked of. §9.1 declares three members and this installs three. */
    container = JS_NewObjectProto(ctx, condition);
    CHECK(!JS_IsException(container), "CSSContainerRule.prototype could not be allocated");
    idl_interface_tag(ctx, container, "CSSContainerRule");
    idl_install_accessor(ctx, container, "containerName", js_rule_get, CR_CONTAINER_NAME, -1);
    idl_install_accessor(ctx, container, "containerQuery", js_rule_get, CR_CONTAINER_QUERY, -1);
    idl_install_accessor(ctx, container, "conditions", js_rule_get, CR_CONTAINER_CONDITIONS, -1);

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

    /* CSS Fonts 5 §9.1's CSSFontFaceRule.prototype. Its `style` is a CSSFontFaceDescriptors and not a
       CSSStyleProperties, which is a real difference a page reads (`[object CSSFontFaceDescriptors]`, and a
       `unicode-range` attribute the other interface does not have) — see core/css/css_style_declaration.h.
       The [PutForwards=cssText] setter is the shared one, because Web IDL §3.3.10 [PutForwards]'s forwarding is
       a [[Get]] of `style` by NAME followed by a [[Set]] of `cssText`, which does not care which interface
       answered. */
    font_face = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(font_face), "CSSFontFaceRule.prototype could not be allocated");
    idl_interface_tag(ctx, font_face, "CSSFontFaceRule");
    idl_install_accessor(ctx, font_face, "style", js_rule_style, STYLE_OF_FONT_FACE,
                         cssom_put_forwards_setter());

    /* §6.4.7's CSSPageRule.prototype. It derives from CSSGroupingRule and not from CSSRule, because an
       `@page` CONTAINS rules — CSS Paged Media §4.3's sixteen margin at-rules — so `cssRules`, `insertRule`
       and `deleteRule` are reachable on one and are exactly the right members for it.
       ITS `selectorText` IS NOT §6.4.3's. The two are two attributes of two interfaces over two grammars: CSS
       Paged Media §4.3's `<page-selector-list>` is not a group of selectors and Selectors admits none of it,
       so the getter reads a different magic and the setter runs a different parse. */
    page = JS_NewObjectProto(ctx, grouping);
    CHECK(!JS_IsException(page), "CSSPageRule.prototype could not be allocated");
    idl_interface_tag(ctx, page, "CSSPageRule");
    idl_install_accessor(ctx, page, "selectorText", js_rule_get, CR_PAGE_SELECTOR_TEXT, g_id_set_page_selector);
    idl_install_accessor(ctx, page, "style", js_rule_style, STYLE_OF_PAGE, cssom_put_forwards_setter());

    /* §6.4.8's CSSMarginRule.prototype — from CSSRule directly, because a margin at-rule's body is CSS Paged
       Media §4.3's `<declaration-list>` and holds no rules at all. */
    margin = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(margin), "CSSMarginRule.prototype could not be allocated");
    idl_interface_tag(ctx, margin, "CSSMarginRule");
    idl_install_accessor(ctx, margin, "name", js_rule_get, CR_MARGIN_NAME, -1);
    idl_install_accessor(ctx, margin, "style", js_rule_style, STYLE_OF_MARGIN, cssom_put_forwards_setter());

    /* CSS Animations §6.3's CSSKeyframesRule.prototype. It derives from CSSRule and NOT from CSSGroupingRule
       even though a `@keyframes` contains rules — the IDL says `interface CSSKeyframesRule : CSSRule` — and
       that is a real difference a page reads: §6.4.5's `insertRule(rule, index)` and index-taking `deleteRule`
       are absent here, and the `deleteRule` that IS here takes a keyframe SELECTOR.
       ITS INDEXED GETTER IS ON THE CLASS, not on this prototype: §6.3.3 declares
       `getter CSSKeyframeRule (unsigned long index)`, which Web IDL §3.9 makes an object's own-property
       behaviour rather than a member. §3.7.10's @@iterator IS a prototype member and goes here, because this
       interface has both an indexed getter and an integer `length`. */
    keyframes = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(keyframes), "CSSKeyframesRule.prototype could not be allocated");
    idl_interface_tag(ctx, keyframes, "CSSKeyframesRule");
    idl_install_accessor(ctx, keyframes, "name", js_rule_get, CR_KEYFRAMES_NAME, g_id_set_keyframes_name);
    idl_install_accessor(ctx, keyframes, "cssRules", js_rule_get, CR_KEYFRAMES_CSS_RULES, -1);
    idl_install_accessor(ctx, keyframes, "length", js_rule_get, CR_KEYFRAMES_LENGTH, -1);
    idl_install_method(ctx, keyframes, "appendRule", g_id_append_rule);
    idl_install_method(ctx, keyframes, "deleteRule", g_id_kf_delete_rule);
    idl_install_method(ctx, keyframes, "findRule", g_id_find_rule);
    idl_indexed_install_iterable(ctx, keyframes);

    /* CSS Animations §6.2's CSSKeyframeRule.prototype — from CSSRule directly, because a `<keyframe-block>`'s
       body is a `<declaration-list>` and holds no rules at all. */
    keyframe = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(keyframe), "CSSKeyframeRule.prototype could not be allocated");
    idl_interface_tag(ctx, keyframe, "CSSKeyframeRule");
    idl_install_accessor(ctx, keyframe, "keyText", js_rule_get, CR_KEY_TEXT, g_id_set_key_text);
    idl_install_accessor(ctx, keyframe, "style", js_rule_style, STYLE_OF_KEYFRAME,
                         cssom_put_forwards_setter());

    /* CSS Cascade §8.1's CSSLayerBlockRule.prototype. It derives from CSSGroupingRule — `interface
       CSSLayerBlockRule : CSSGroupingRule` — and §6.4.4.1 gives the reason the IDL encodes: "such @layer block
       rules have the same restrictions and processing as a conditional group rule [CSS-CONDITIONAL-3] with a
       TRUE condition". So `cssRules`, `insertRule` and `deleteRule` are reachable on one and are exactly the
       right members for it. It is NOT a CSSConditionRule, and that is the half of the sentence that matters
       here: a layer has no condition to read back, so CSS Conditional §7.2's `conditionText` is absent and the
       prototype chains to CSSGroupingRule directly, exactly as §6.4.7's CSSPageRule does. */
    layer_block = JS_NewObjectProto(ctx, grouping);
    CHECK(!JS_IsException(layer_block), "CSSLayerBlockRule.prototype could not be allocated");
    idl_interface_tag(ctx, layer_block, "CSSLayerBlockRule");
    idl_install_accessor(ctx, layer_block, "name", js_rule_get, CR_LAYER_BLOCK_NAME, -1);

    /* CSS Cascade §8.2's CSSLayerStatementRule.prototype — from CSSRule directly, because §6.4.4.2's at-rule
       has no block at all and therefore contains no rules: `interface CSSLayerStatementRule : CSSRule`. Its
       one member is a `FrozenArray<CSSOMString>`, whose freeze is on the stored VALUE (see the record). */
    layer_statement = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(layer_statement), "CSSLayerStatementRule.prototype could not be allocated");
    idl_interface_tag(ctx, layer_statement, "CSSLayerStatementRule");
    idl_install_accessor(ctx, layer_statement, "nameList", js_rule_get, CR_LAYER_NAME_LIST, -1);

    /* CSS Properties and Values API 1 §6.1's CSSPropertyRule.prototype — from CSSRule directly, because §6.1
       declares `interface CSSPropertyRule : CSSRule` and an `@property` body is §3's `<declaration-list>` with
       no rule in it. ITS FOUR MEMBERS ARE THE WHOLE INTERFACE, and the one that is NOT there is the point:
       §6.1's IDL has no `style`, so an `@property` rule's descriptors are not reachable as a §6.6 declaration
       block and the three that exist are read through attributes of their own. `initialValue` is the only
       nullable one, which is §3.3's initial showing through. */
    property_rule = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(property_rule), "CSSPropertyRule.prototype could not be allocated");
    idl_interface_tag(ctx, property_rule, "CSSPropertyRule");
    idl_install_accessor(ctx, property_rule, "name", js_rule_get, CR_PROPERTY_NAME, -1);
    idl_install_accessor(ctx, property_rule, "syntax", js_rule_get, CR_PROPERTY_SYNTAX, -1);
    idl_install_accessor(ctx, property_rule, "inherits", js_rule_get, CR_PROPERTY_INHERITS, -1);
    idl_install_accessor(ctx, property_rule, "initialValue", js_rule_get, CR_PROPERTY_INITIAL_VALUE, -1);

    /* Each into the realm's own slot, which asserts on its own that this install ran once in this realm. */
    realm_value_set(ctx, g_proto_slot[PROTO_PROPERTY], property_rule);
    realm_value_set(ctx, g_proto_slot[PROTO_KEYFRAMES], keyframes);
    realm_value_set(ctx, g_proto_slot[PROTO_KEYFRAME], keyframe);
    realm_value_set(ctx, g_proto_slot[PROTO_LAYER_BLOCK], layer_block);
    realm_value_set(ctx, g_proto_slot[PROTO_LAYER_STATEMENT], layer_statement);
    realm_value_set(ctx, g_proto_slot[PROTO_IMPORT], import_rule);
    realm_value_set(ctx, g_proto_slot[PROTO_NAMESPACE], ns);
    realm_value_set(ctx, g_proto_slot[PROTO_FONT_FACE], font_face);
    realm_value_set(ctx, g_proto_slot[PROTO_PAGE], page);
    realm_value_set(ctx, g_proto_slot[PROTO_MARGIN], margin);
    realm_value_set(ctx, g_proto_slot[PROTO_RULE], base);
    realm_value_set(ctx, g_proto_slot[PROTO_GROUPING], grouping);
    realm_value_set(ctx, g_proto_slot[PROTO_STYLE], style);
    realm_value_set(ctx, g_proto_slot[PROTO_CONDITION], condition);
    realm_value_set(ctx, g_proto_slot[PROTO_MEDIA], media);
    realm_value_set(ctx, g_proto_slot[PROTO_SUPPORTS], supports);
    realm_value_set(ctx, g_proto_slot[PROTO_CONTAINER], container);
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
        /* CSS Conditional §7.4: `interface CSSSupportsRule : CSSConditionRule` — index 3, the SAME parent
           CSSMediaRule has, because the two conditional at-rules are siblings and not a chain. */
        { "CSSSupportsRule",   PROTO_SUPPORTS,   3 },
        /* CSS Conditional 5 §9.1: `interface CSSContainerRule : CSSConditionRule` — index 3 again, the THIRD
           sibling under it. */
        { "CSSContainerRule",  PROTO_CONTAINER,  3 },
        /* Each of the three derives from CSSRule directly — none of them contains rules. */
        { "CSSImportRule",     PROTO_IMPORT,     0 },
        { "CSSNamespaceRule",  PROTO_NAMESPACE,  0 },
        { "CSSFontFaceRule",   PROTO_FONT_FACE,  0 },
        /* §6.4.7's derives from CSSGroupingRule (index 1) — an `@page` contains §4.3's margin at-rules —
           while §6.4.8's derives from CSSRule, because a margin at-rule contains none. */
        { "CSSPageRule",       PROTO_PAGE,       1 },
        { "CSSMarginRule",     PROTO_MARGIN,     0 },
        /* CSS Animations §6.2.1 and §6.3.1 declare both as `: CSSRule`. A `@keyframes` holds rules and is
           still not a CSSGroupingRule, which is the IDL's own statement and not a simplification. */
        { "CSSKeyframeRule",   PROTO_KEYFRAME,   0 },
        { "CSSKeyframesRule",  PROTO_KEYFRAMES,  0 },
        /* CSS Cascade §8.1 derives from CSSGroupingRule (index 1) — §6.4.4.1 makes a `@layer` block a
           conditional group rule with a true condition — while §8.2's derives from CSSRule, because a
           statement at-rule has no block and so contains nothing. */
        { "CSSLayerBlockRule",     PROTO_LAYER_BLOCK,     1 },
        { "CSSLayerStatementRule", PROTO_LAYER_STATEMENT, 0 },
        /* CSS Properties and Values API 1 §6.1 declares `interface CSSPropertyRule : CSSRule` — index 0 —
           because an `@property` contains no rules and has no declaration block a page can reach. */
        { "CSSPropertyRule",       PROTO_PROPERTY,        0 },
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
