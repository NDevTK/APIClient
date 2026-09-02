/* CSSOM §6.1 — the CSS STYLE SHEET object. See css_style_sheet.h for what this owns and what it deliberately
 * does not.
 *
 * THE RECORD TIME-TRAVELS, AND THE DISABLED FLAG IS WHY. §6.1.1's `disabled` is the one state item a page
 * WRITES on a sheet it did not create, and it is written into a C record behind a class opaque where no
 * property hook and no engine hook can see it — so a forked arm that set `sheet.disabled = true` would have
 * disabled that sheet for its sibling, and for every flow the frontier resumes afterwards. The capture is in
 * the ACCESSOR (`sheet_of`), which is the one rule that makes this impossible to get wrong: a record a flow
 * has REACHED is one it may write, the delta dedups to a single entry, and there is then no write site left to
 * miss. `SHEET_VALS` names the record's owned JSValues and is the SAME list `sheet_finalizer` frees and
 * `sheet_gc_mark` marks — read the three together, because a field added to one and not the others is exactly
 * the bug the layout exists to prevent.
 *
 * THE TITLE IS READ LIVE OFF THE OWNER NODE WHILE THERE IS ONE, AND FROZEN WHEN THERE IS NOT. §6.1 says the
 * title is "specified when created", and then that when it is specified to an ATTRIBUTE of the owner node it
 * "must be set to the new value of the attribute" whenever that attribute is set, changed or removed — a value
 * kept in sync, which is the same function as reading it at the ask. §6.2's remove-a-CSS-style-sheet nulls the
 * owner node and does NOT touch the title, so at that one moment the live read stops being available and the
 * value it would have produced is written into the record. The two halves compute the same number; the second
 * exists because after a removal there is nothing left to read it from.
 *
 * ONE CLASS, TWO PROTOTYPES. Every sheet this engine can build is a CSSStyleSheet — §6.1.1's StyleSheet is an
 * "abstract, base style sheet" that nothing instantiates — so there is one class id, and StyleSheet.prototype
 * is a per-realm value in quickjs's own context-slot array (core/realm.h) rather than a second class's
 * prototype slot. Both are PER REALM because §3.7 makes them so and because a C member runs in the realm that
 * DEFINED it: one shared prototype would answer every document's `sheet.title` out of whichever realm happened
 * to build it first. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_rule.h"
#include "core/css/css_rule_list.h"
#include "core/css/css_style_sheet.h"
#include "core/css/media_list.h"   /* §6.1's media state item IS a §4.4 MediaList object */
#include "core/css/style_sheet_list.h"
#include "core/dom/document.h"   /* §6.1's constructor step 2 reads the realm's document base URL */
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"   /* §6.1 step 12 asserts what it cannot parse */
#include "solver/cow.h"

/* §6.1's state items, as far as anything reads them — see the header for why the other eight are not here. */
typedef struct CssStyleSheetData {
    JSValue owner_node;          /* §6.1 "owner node" — an Element wrapper, or JS_NULL (OWNED) */
    JSValue parent_style_sheet;  /* §6.1 "parent CSS style sheet" (OWNED) */
    JSValue owner_rule;          /* §6.1 "owner CSS rule" (OWNED) */
    JSValue location;            /* §6.1 "location" — a USVString, or JS_NULL when embedded (OWNED) */
    /* §6.1 "title", used ONLY once the owner node is gone — while there is one the title is read off its
       attribute, which is what "set to the new value of the attribute" means. Written by the removal. */
    JSValue title;               /* (OWNED) */
    /* §6.1's "CSS RULES". A JS ARRAY of §6.4 rule objects, and that is the constraint the whole component turns
       on: lexbor parses into an arena and names every rule by a pointer into it, which has no cross-tier
       identity — a sheet holding one could neither park to the IDB cold tier nor fork per flow. An Array's
       mutations are property writes the delta already captures, and `insertRule`/`deleteRule` are exactly the
       mutations two flows must be able to disagree about. */
    JSValue rules;               /* (OWNED) */
    /* §6.1.2's `[SameObject] readonly attribute CSSRuleList cssRules` — the collection over `rules`, minted
       once because a page holds it and compares it. It shares the very Array above, which is what its
       liveness IS. */
    JSValue rule_list;           /* (OWNED) */
    /* §6.1 "media" — "Specified when created. The MediaList object associated with the CSS style sheet." A
       §4.4 MediaList OBJECT and not a string, because §6.1.1 declares the attribute `[SameObject]`: a page
       holds `sheet.media` and compares it, and `[PutForwards=mediaText]` writes THROUGH it, so re-minting one
       per read would hand back a different object each time AND lose every write. Both specifications §6.1
       gives reduce to §4.4's create a MediaList object with a string — the creator hands over that string and
       the mint below is where the object is made. */
    JSValue media;               /* (OWNED) */
    bool    disabled;            /* §6.1 "disabled flag" */
    /* §6.1 "constructed flag". A POD latch and NOT a JSValue, so it is not in SHEET_VALS — the layout names the
       record's owned JSValues, and this is the same kind of field `disabled` is. It is written once, by §6.1's
       create a constructed CSSStyleSheet, and never again: no member of either interface sets or clears it, so
       it does not have to time-travel independently of the record the accessor already captures. */
    bool    constructed;
} CssStyleSheetData;

static JSClassID g_sheet_class;
static int       g_stylesheet_proto_slot = -1;   /* StyleSheet.prototype, per realm */
static int       g_id_set_disabled = -1, g_id_insert_rule = -1, g_id_delete_rule = -1;
static int       g_id_ctor = -1;                 /* §6.1's `constructor(optional CSSStyleSheetInit options)` */

/* WHAT THE RECORD OWNS. One list, three readers: the COW layout below, the finalizer, and the gc_mark. */
static const uint16_t SHEET_VALS[] = {
    (uint16_t)offsetof(CssStyleSheetData, owner_node),
    (uint16_t)offsetof(CssStyleSheetData, parent_style_sheet),
    (uint16_t)offsetof(CssStyleSheetData, owner_rule),
    (uint16_t)offsetof(CssStyleSheetData, location),
    (uint16_t)offsetof(CssStyleSheetData, title),
    (uint16_t)offsetof(CssStyleSheetData, rules),
    (uint16_t)offsetof(CssStyleSheetData, rule_list),
    (uint16_t)offsetof(CssStyleSheetData, media),
};
static const CowRecord SHEET_REC = { sizeof(CssStyleSheetData), SHEET_VALS, 8 };

/* THE ACCESSOR, AND THEREFORE THE CAPTURE POINT. Every member of both interfaces reaches the record through
   here, so every flow that can write the disabled flag has already had its delta take a copy of the record. */
static CssStyleSheetData *sheet_of(JSValueConst v)
{
    CssStyleSheetData *s = JS_GetOpaque(v, g_sheet_class);

    if (s) cow_capture_host_record(v, s, &SHEET_REC);
    return s;
}

/* WRITE ONE OWNED SLOT, and never `JS_FreeValue(ctx, s->f); s->f = <a new value>;` — see cow.h for the order
   and the defect. §6.2's remove a CSS style sheet is the worst shape of it this component has, and it is worse
   than the ordinary pair: it freed THREE slots and then assigned all three, so between the first free and its
   assignment sat two more JS_FreeValues, each able to run a host finalizer — the page's platform code, which
   may allocate — and an allocation IS a collection (js_trigger_gc has exactly one caller,
   JS_NewObjectFromShape) that reaches this record through sheet_gc_mark and decrefs a JSObject already back on
   the allocator's free list. Giving `owner_node` back is exactly that: it can drop the last reference to an
   element wrapper. Routing each write through one operation is what makes the window impossible rather than a
   property of how the three lines happen to be grouped.
   The record and its layout are bound HERE rather than at each call, so no site can pass a slot from another
   record with this layout. css_style_sheet_new's mint does not come here: before JS_SetOpaque the record is
   unreachable by the collector and its calloc'd slots hold no value to release. */
/* THE ADDRESS PASSES THROUGH: the asserts inside are about the SLOT, so they must name the WRITE and not this
   line — see cow.h's THE SITE TRAVELS WITH THE OPERATION. */
static void sheet_set_at(JSContext *ctx, CssStyleSheetData *s, JSValue *slot, JSValue v,
                         const char *file, int line)
{
    cow_record_set_at(ctx, s, &SHEET_REC, slot, v, file, line);
}
#define sheet_set(ctx_, s_, slot_, v_) sheet_set_at((ctx_), (s_), (slot_), (v_), __FILE__, __LINE__)

/* The receiver, brand-checked. Both interfaces declare every member on a PROTOTYPE, so a page can apply one to
   anything at all, and the answer — Web IDL §3.7.6 Attributes' for the accessors, §3.7.7 Operations' for
   `insertRule`, `deleteRule`, `replace` and `replaceSync` — is a TypeError rather than a read of nothing. */
static CssStyleSheetData *sheet_here(JSContext *ctx, JSValueConst v)
{
    CssStyleSheetData *s = sheet_of(v);

    if (!s) {
        JS_ThrowTypeError(ctx, "not a CSSStyleSheet");
        return NULL;
    }
    return s;
}

bool css_style_sheet_is(JSValueConst v)
{
    DCHECK(g_sheet_class != 0, "a value was asked whether it is a CSSStyleSheet before the interface existed");
    return JS_GetOpaque(v, g_sheet_class) != NULL;
}

/* Finalizers and gc_marks go through JS_GetOpaque rather than the accessor DELIBERATELY: a capture during
   collection would dup values on an object being torn down. */
static void sheet_finalizer(JSRuntime *rt, JSValue val)
{
    CssStyleSheetData *s = JS_GetOpaque(val, g_sheet_class);

    if (!s) return;
    JS_FreeValueRT(rt, s->owner_node);
    JS_FreeValueRT(rt, s->parent_style_sheet);
    JS_FreeValueRT(rt, s->owner_rule);
    JS_FreeValueRT(rt, s->location);
    JS_FreeValueRT(rt, s->title);
    JS_FreeValueRT(rt, s->rules);
    JS_FreeValueRT(rt, s->rule_list);
    JS_FreeValueRT(rt, s->media);
    free(s);
}

static void sheet_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    CssStyleSheetData *s = JS_GetOpaque(val, g_sheet_class);

    if (!s) return;
    JS_MarkValue(rt, s->owner_node, mark_func);
    JS_MarkValue(rt, s->parent_style_sheet, mark_func);
    JS_MarkValue(rt, s->owner_rule, mark_func);
    JS_MarkValue(rt, s->location, mark_func);
    JS_MarkValue(rt, s->title, mark_func);
    JS_MarkValue(rt, s->rules, mark_func);
    JS_MarkValue(rt, s->rule_list, mark_func);
    JS_MarkValue(rt, s->media, mark_func);
}

/* ---- §6.1's TITLE ---------------------------------------------------------------------------------------- */

/* The title this sheet HAS — the concept, not §6.1.1's attribute, which turns the empty string into null.
   OWNED (a JS string).
   HTML §4.2.6 specifies the title of a `<style>`-created sheet as "the title attribute of element, if element
   is in a document tree, or the empty string otherwise", and that condition is asked HERE rather than frozen at
   creation because HTML makes it a reference to the attribute: `styleEl.title = 'x'` changes the sheet's title
   in a real browser, and a copy taken at creation would not. */
static JSValue sheet_title_concept(JSContext *ctx, const CssStyleSheetData *s)
{
    lxb_dom_node_t *n = node_of(s->owner_node);
    size_t len = 0;
    const lxb_char_t *v;

    if (!n) return JS_DupValue(ctx, s->title);   /* removed: the value the live read last produced */
    DCHECK(n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "a CSS style sheet's owner node is neither an element nor null — §6.1.1 types it as `(Element or "
           "ProcessingInstruction)?`, and this engine builds no ProcessingInstruction-owned sheet, so anything "
           "else means a creator handed over something that is not an owner node at all");
    if (!node_is_connected(n)) return JS_NewStringLen(ctx, "", 0);
    v = lxb_dom_element_get_attribute(lxb_dom_interface_element(n), (const lxb_char_t *)"title", 5, &len);
    return v ? JS_NewStringLen(ctx, (const char *)v, len) : JS_NewStringLen(ctx, "", 0);
}

/* ---- §6.1.1's StyleSheet ---------------------------------------------------------------------------------- */

enum { SS_TYPE = 0, SS_HREF, SS_OWNER_NODE, SS_PARENT_STYLE_SHEET, SS_TITLE, SS_DISABLED, SS_OWNER_RULE,
       SS_MEDIA };

static JSValue js_sheet_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssStyleSheetData *s = sheet_here(ctx, this_val);

    if (!s) return JS_EXCEPTION;
    switch (magic) {
    /* "The type attribute must return the type", and §6.1's type is "the literal string text/css" for every
       CSS style sheet there is — it is not a property a creator chooses. */
    case SS_TYPE: return JS_NewString(ctx, "text/css");
    /* "The href attribute must return the location." */
    case SS_HREF: return JS_DupValue(ctx, s->location);
    case SS_OWNER_NODE: return JS_DupValue(ctx, s->owner_node);
    case SS_PARENT_STYLE_SHEET: return JS_DupValue(ctx, s->parent_style_sheet);
    case SS_TITLE: {
        /* "must return the title or null if title is the empty string" — the one member whose null and whose
           empty string are different answers, which is why the concept above is not the attribute.
           The emptiness test is on the LENGTH and not on a leading NUL byte: a title is a DOMString and may
           hold U+0000, so `!*c` would report `"\0x"` as the empty string and answer null for a real title. */
        JSValue t = sheet_title_concept(ctx, s);
        size_t len = 0;
        const char *c = JS_ToCStringLen(ctx, &len, t);

        if (!c) { JS_FreeValue(ctx, t); return JS_EXCEPTION; }
        JS_FreeCString(ctx, c);
        if (len) return t;
        JS_FreeValue(ctx, t);
        return JS_NULL;
    }
    /* "on getting, must return true if the disabled flag is set, or false otherwise" */
    case SS_DISABLED: return JS_NewBool(ctx, s->disabled);
    /* §6.1.1: "The media attribute must return the media." The state item ITSELF and never a copy — that is
       what `[SameObject]` means, and it is also what makes `[PutForwards=mediaText]` reach the sheet: the
       forwarding does a real [[Get]] of `media` and then sets `mediaText` on what it got, so a fresh object
       here would take the write and be dropped on the next line. */
    case SS_MEDIA:
        DCHECK(media_list_is(ctx, s->media),
               "a CSS style sheet's media is not a MediaList — §6.1 says the state item is \"specified when "
               "created\" and every creator mints one through §4.4's create a MediaList object, so this sheet "
               "was built by a path that skipped it");
        return JS_DupValue(ctx, s->media);
    default:
        DCHECK(magic == SS_OWNER_RULE,
               "a style sheet attribute ran with a magic neither §6.1.1 nor §6.1.2 declares");
        /* §6.1.2: "The ownerRule attribute must return the owner CSS rule." Null for every sheet this engine
           can build — only an @import rule names one — and it is READ off the record rather than answered as a
           constant, so the day CSSImportRule's creator writes one this member already reports it. */
        return JS_DupValue(ctx, s->owner_rule);
    }
}

/* §6.1.1: "On setting, the disabled attribute must set the disabled flag if the new value is true, or unset the
   disabled flag otherwise." The value arrives already converted — `boolean` is the declaration's work. */
static JSValue js_sheet_set_disabled(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    CssStyleSheetData *s = sheet_here(ctx, this_val);

    (void)magic;
    if (!s) return JS_EXCEPTION;
    s->disabled = JS_ToBool(ctx, val) != 0;
    return JS_UNDEFINED;
}

/* The flag BY NAME, for HTML §4.2.6's forwarding. There is no null arm past the assertion and there must not be
   one: every caller has already asked css_style_sheet_is, so a NULL here is a caller that skipped the brand,
   and answering `false` for it would report a live sheet as enabled forever. */
bool css_style_sheet_disabled(JSValueConst sheet)
{
    CssStyleSheetData *s = sheet_of(sheet);

    DCHECK(s != NULL, "the disabled flag was read off something that is not a CSS style sheet — the callers "
                      "that forward to it hold a value they took off an association slot, and that slot holds "
                      "a sheet or nothing");
    return s->disabled;
}

void css_style_sheet_set_disabled(JSValueConst sheet, bool disabled)
{
    CssStyleSheetData *s = sheet_of(sheet);

    DCHECK(s != NULL, "the disabled flag was written on something that is not a CSS style sheet");
    s->disabled = disabled;
}

/* §6.1's CONSTRUCTED FLAG, BY NAME, for §6.1.2's insertRule step 5 — see the header. There is no null arm past
   the assertion for the same reason the disabled flag has none: the one caller reaches this with the value
   §6.1.2's own `sheet_here` already brand-checked, so a NULL here is a caller that skipped the brand, and
   answering `false` for it would let an @import into a constructed sheet for ever. */
bool css_style_sheet_constructed(JSValueConst sheet)
{
    CssStyleSheetData *s = sheet_of(sheet);

    DCHECK(s != NULL, "the constructed flag was read off something that is not a CSS style sheet — §6.1.2's "
                      "insertRule is its one reader and it holds the receiver its own brand check passed");
    return s->constructed;
}

/* §6.2's two questions about a sheet it is placing. Both go through the capturing accessor like every other
   read, and both assert the brand rather than answering for a stranger. */
lxb_dom_node_t *css_style_sheet_owner_node(JSValueConst sheet)
{
    CssStyleSheetData *s = sheet_of(sheet);

    DCHECK(s != NULL, "the owner node was read off something that is not a CSS style sheet");
    return node_of(s->owner_node);
}

/* §6.1's CSS RULES, as the very Array `cssRules` shares — the CASCADE reads the rule OBJECTS through here, so
   an `insertRule` the running flow made is in the cascade that flow resolves and in no sibling's. Through the
   capturing accessor like every other read. OWNED. */
JSValue css_style_sheet_rules(JSContext *ctx, JSValueConst sheet)
{
    CssStyleSheetData *s = sheet_of(sheet);

    DCHECK(s != NULL, "the CSS rules were read off something that is not a CSS style sheet");
    DCHECK(JS_IsArray(s->rules),
           "a CSS style sheet's rules are not an Array — the create allocates one before the sheet is handed "
           "to anybody, and nothing replaces it");
    return JS_DupValue(ctx, s->rules);
}

JSValue css_style_sheet_title(JSContext *ctx, JSValueConst sheet)
{
    CssStyleSheetData *s = sheet_of(sheet);

    DCHECK(s != NULL, "the title was read off something that is not a CSS style sheet");
    return sheet_title_concept(ctx, s);
}

/* ---- §6.2's "create a CSS style sheet" and "remove a CSS style sheet" ------------------------------------ */

/* THE OBJECT AND ITS STATE ITEMS, WITHOUT ANY DECISION ABOUT WHERE THE SHEET THEN GOES. §6.2's create-a-CSS-
   style-sheet is this plus its own step 2 (the add); §6.1's create-a-constructed-CSSStyleSheet is this and
   NOTHING ELSE, because a constructed sheet is in no collection at all. Factoring it this way rather than
   giving the create a `bool add` is the difference between two algorithms sharing a mint and one algorithm
   with a switch in it: the add is step 2 of §6.2 and of no other standard, so it belongs to §6.2's function.
   `disabled` and `constructed` are parameters and not post-mint writes: the record is unreachable by the
   collector until JS_SetOpaque, so every state item this sheet is born with is written in one place. */
static JSValue sheet_mint(JSContext *ctx, JSValueConst owner_node, JSValueConst parent_style_sheet,
                          JSValueConst owner_rule, JSValueConst location, const char *media_text,
                          bool constructed, bool disabled)
{
    JSValue proto, obj;
    CssStyleSheetData *s;

    DCHECK(g_sheet_class != 0, "a CSS style sheet was created before css_style_sheet_init declared the class");
    DCHECK(JS_IsNull(location) || JS_IsString(location),
           "§6.1's location is \"the absolute-URL string of the first request of the CSS style sheet or null if "
           "the CSS style sheet was embedded\" — a creator handed over neither");
    proto = JS_GetClassProto(ctx, g_sheet_class);
    DCHECK(!JS_IsNull(proto), "a CSS style sheet was created in a realm with no CSSStyleSheet.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_sheet_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    s = calloc(1, sizeof(*s));
    CHECK(s != NULL, "the CSS style sheet record allocation failed");
    s->owner_node = JS_DupValue(ctx, owner_node);
    s->parent_style_sheet = JS_DupValue(ctx, parent_style_sheet);
    s->owner_rule = JS_DupValue(ctx, owner_rule);
    s->location = JS_DupValue(ctx, location);
    /* Not the title the sheet HAS — the slot the removal freezes the live read into. While there is an owner
       node this value is never consulted, which is what sheet_title_concept states from the other side. */
    s->title = JS_NewStringLen(ctx, "", 0);
    /* §6.1's CSS rules start EMPTY and are filled by the creator — HTML §4.2.6's is
       css_style_sheet_set_rules_from_text, below. The Array exists from the first instant so that `cssRules`
       never has to mint one lazily inside a read. */
    s->rules = JS_NewArray(ctx);
    CHECK(!JS_IsException(s->rules), "a CSS style sheet's rule list could not be allocated");
    s->rule_list = JS_UNDEFINED;
    /* §6.1's MEDIA, "specified when created" — §4.4's CREATE A MEDIALIST OBJECT with the string the creator
       specified. NULL is the empty string, which is the empty collection and therefore a sheet that applies to
       every medium: that is what an absent `media` content attribute specifies and what §6.1.2's
       `(MediaList or DOMString) media = ""` default specifies, so the two creators agree without either of
       them saying so. It is minted BEFORE JS_SetOpaque like every other slot — the record is unreachable by
       the collector until then, so this allocation cannot reach sheet_gc_mark through a half-built record. */
    s->media = media_list_new(ctx, media_text);
    CHECK(!JS_IsException(s->media), "a CSS style sheet's media list could not be allocated");
    /* §6.1: the disabled flag is "either set or unset. Unset by default". §6.2's create specifies no value for
       it, so every creator but one leaves it at the calloc's zero; the one that does specify it is §6.1's
       constructor, whose step 13 is "If the disabled attribute of options is true, set sheet's disabled flag".
       The assert is what keeps the parameter from becoming a second way for an ordinary create to set it. */
    DCHECK(constructed || !disabled,
           "§6.2's create a CSS style sheet asked for a sheet whose disabled flag is already set — §6.1 gives "
           "that flag no creator-specified value, and the only algorithm in CSSOM that sets it at creation is "
           "§6.1's create a constructed CSSStyleSheet");
    s->disabled = disabled;
    s->constructed = constructed;
    JS_SetOpaque(obj, s);
    return obj;
}

JSValue css_style_sheet_create(JSContext *ctx, JSValueConst owner_node, JSValueConst parent_style_sheet,
                               JSValueConst owner_rule, JSValueConst location, const char *media)
{
    /* STEP 1 — "Create a new CSS style sheet object and set its properties as specified." */
    JSValue obj = sheet_mint(ctx, owner_node, parent_style_sheet, owner_rule, location, media,
                             /*constructed*/ false, /*disabled*/ false);

    if (JS_IsException(obj)) return obj;
    /* STEP 2 — "Then run the add a CSS style sheet steps for the newly created CSS style sheet."
       The sheet is COMPLETE before this line: the add reads the owner node to decide where in tree order the
       sheet belongs, and reads the title to decide what the style-sheet-set steps have to say about it. */
    style_sheet_list_add(ctx, obj, owner_node);
    return obj;
}

void css_style_sheet_remove(JSContext *ctx, JSValueConst sheet)
{
    /* THE BRAND IS ASSERTED, NOT THROWN. This is an algorithm another standard invokes on a sheet it already
       holds — never a member a page can apply to a stranger — so a non-sheet here is an engine bug, and
       sheet_here's TypeError would leave a pending exception in a C caller with no member to return it from. */
    CssStyleSheetData *s = sheet_of(sheet);
    JSValue title;

    DCHECK(s != NULL, "§6.2's remove a CSS style sheet was invoked on something that is not a CSS style sheet");
    /* STEP 1 — "remove the CSS style sheet from the list of document or shadow root CSS style sheets". It runs
       BEFORE step 2 nulls the owner node, and that order is load-bearing rather than incidental: the list a
       sheet is in is the one its add recorded, and step 2 is what makes the sheet stop naming a tree at all. */
    style_sheet_list_remove(ctx, sheet);
    /* THE TITLE IS FROZEN BEFORE THE OWNER NODE GOES, which is the whole of why the record carries one: §6.2
       nulls the owner node and says nothing about the title, so after this line there is nothing left to read
       it off and the last value the live read produced IS the title from here on. */
    title = sheet_title_concept(ctx, s);
    sheet_set(ctx, s, &s->title, title);
    /* Step 2. */
    sheet_set(ctx, s, &s->owner_node, JS_NULL);
    sheet_set(ctx, s, &s->parent_style_sheet, JS_NULL);
    sheet_set(ctx, s, &s->owner_rule, JS_NULL);
}

/* ---- §6.1.2's CSS RULES: cssRules, insertRule and deleteRule ---------------------------------------------- */

/* §6.4's own list operations, its INSERT A CSS RULE and its REMOVE A CSS RULE all live in core/css/css_rule.c,
   because the spec states each of them ONCE over "a CSS rule list" and §6.4.5's CSSGroupingRule is stated over
   the very same three. A private copy here is what made §6.1.2's insertRule and §6.4.5's two implementations of
   one algorithm, which is exactly the pair that drifts. `rules_len` is the one thing this file still needs of
   its own — it asks its own list a question, and asking css_rule.c for it would export a list primitive nothing
   else wants. */
static uint32_t rules_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

void css_style_sheet_set_rules_from_text(JSContext *ctx, JSValueConst sheet, const char *text, size_t len)
{
    CssStyleSheetData *s = sheet_of(sheet);

    DCHECK(s != NULL, "a sheet's CSS rules were set on something that is not a CSS style sheet");
    DCHECK(rules_len(ctx, s->rules) == 0,
           "a CSS style sheet's rules were parsed into a list that is not empty — this is the operation "
           "§6.1.2's replaceSync is stated over and it SETS the rules, so a second call would append to the "
           "first instead of replacing it");
    css_rule_build_sheet(ctx, s->rules, sheet, text, len);
}

/* §6.1.2's `cssRules`. Its steps 1-2 are an origin-clean check and then "return a read-only, LIVE CSSRuleList
   object representing the CSS rules". The origin-clean flag is not modelled — every sheet this build creates is
   one HTML §4.2.6 sets it for, and the flag exists to distinguish a cross-origin `<link>`, which is a sheet
   nothing here can make. [SameObject] is why the collection is remembered on the record. */
static JSValue js_sheet_css_rules(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssStyleSheetData *s = sheet_here(ctx, this_val);

    (void)magic;
    if (!s) return JS_EXCEPTION;
    if (!JS_IsObject(s->rule_list))
        s->rule_list = css_rule_list_new(ctx, JS_DupValue(ctx, s->rules));
    return JS_DupValue(ctx, s->rule_list);
}

/* §6.1.2's `insertRule(rule, index)`: its step 6 is "return the result of invoking insert a CSS rule rule in the
   CSS rules at index" — WITHOUT the nested flag, which is the whole difference from §6.4.5's, and which is why
   the algorithm itself lives in core/css/css_rule.c and this member only names its arguments.
   ITS STEP 5 IS THERE TOO AND NOT HERE. "If parsed rule is an @import rule, and the constructed flag is set,
   throw a SyntaxError DOMException" is a question about the PARSED RULE, which exists only inside that
   algorithm; §6.1.2 parses at step 3 to ask it and then passes the TEXT to step 6 to be parsed again, so asking
   it once, where the rule is, is the same answer. `css_style_sheet_constructed` is how the fact reaches it. */
static JSValue js_sheet_insert_rule(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic)
{
    CssStyleSheetData *s = sheet_here(ctx, this_val);
    const char *text;
    uint32_t index = 0;
    JSValue out;

    (void)magic;
    if (!s) return JS_EXCEPTION;
    DCHECK(argc >= 1, "§6.1.2's insertRule reached its body with no rule — its first IDL argument is required");
    /* Both arguments arrive CONVERTED — `CSSOMString rule` and `optional unsigned long index = 0` are the
       declaration's work — OR AS UNKNOWN EXTERNAL INPUT, which crosses a Web IDL §3.2 conversion AS ITSELF so
       that opacity survives it. This comment used to stop at the first clause and conclude "so nothing here
       runs the page's code", under which a raw `JS_ToUint32` of the index stood; §3.2's conversion is a
       BOUNDARY and never a guarantee that what arrives is a Number. CSS_RULE_INSERT_INDEX reads the known
       value through the one copy of the arithmetic and names the fork it cannot yet perform. */
    if (argc >= 2) CSS_RULE_INSERT_INDEX(ctx, index, argv[1], "§6.1.2's `insertRule`");
    text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    /* A rule at a SHEET's top level has no enclosing rule, which is §6.4.2's parent CSS rule being null. */
    out = css_rule_list_insert(ctx, s->rules, this_val, JS_NULL, index, text, /*nested*/ false);
    JS_FreeCString(ctx, text);
    return out;
}

/* §6.1.2's `deleteRule(index)` STEP 3: "Remove a CSS rule in the CSS rules at index." That is the LAST of its
 * three steps — step 1 is the origin-clean check and step 2 the disallow-modification flag — and neither of the
 * first two is modelled: every sheet this build creates is one HTML §4.2.6 sets the origin-clean flag for (see
 * `js_sheet_css_rules` above, which says the same of its own step 1), and §6.1.2's `replace` — the ONE
 * algorithm in CSSOM whose steps say "Set the disallow modification flag" — is not a member of this engine at
 * all, so no sheet in this build has ever had that flag set for anything to read. That sentence used to defer
 * instead to whatever `css_style_sheet_install` said about the constructor `replace` hangs off, and what it
 * said there was that the constructor did not exist. It does now, so the deferral is retired and `replace` is
 * absent for its OWN reason: its steps settle a promise from work done in parallel, which is a scheduler flow
 * and not a member body.
 *
 * IT IS A STEP MACHINE BECAUSE ITS ONE ARGUMENT CAN BE UNKNOWN, AND A PLAIN BODY CANNOT ASK. `JS_ToUint32` on
 * `argv[0]` stood here — the shape core/idl_args.h bans by name ("A BODY MAY NOT CALL JS_ToFloat64 ON ITS OWN
 * ARGUMENT") — and Web IDL §3.2's conversion is a BOUNDARY unknown external input crosses AS ITSELF, so
 * `sheet.deleteRule(location.hash.length)` reached that line still holding the unknown and the coercion owed C
 * a number it cannot have. ToNumber hands a concolic straight back, so the engine aborts INSIDE the coercion,
 * one frame below this file: checking its return would have been no defence, because there is no return to
 * check. The known value goes through core/idl_index_arg.h's `idl_index_arg_known` — §3.2's one reader for a
 * body that needs a real number, plus §3.2.4.6's own postconditions, written once for every member of this
 * family instead of once per member — and the discarded return is REMOVED rather than asserted dead.
 * THE UNKNOWN IS §6.4's OWN FORK — css_rule_delete_index_run holds the question, and it lives beside the
 * algorithm in core/css/css_rule.c because §6.4.5's `deleteRule` is stated over the same one; the elimination
 * chain underneath it is core/idl_index_arg.h's, shared with the whole `item(index)` family. */
#define SD_STAGES(X)                                                                                          \
    X(SD_REMOVE,                                                                                              \
      "CSSOM §6.1.2 The CSSStyleSheet Interface deleteRule(index) step 3 (remove a CSS rule in the CSS rules "  \
      "at index)")
enum { IDL_STEP_STAGE_BASE(SD_STAGES) SD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SD_STEPS[] = { SD_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* NOTHING IS OWNED, so the visit names nothing — the state is the chain's cursor and the buffer its key is
   spelled into, neither of them a JSValue. It is DECLARED rather than omitted because a machine with no
   `visit` cannot be forked and is refused at registration, and forking is the whole of what this one is for. */
static void sd_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int js_sheet_delete_rule(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdlIndexChain *c = state;
    CssStyleSheetData *s;
    uint32_t index = 0;
    JSValue out;
    int rc;

    (void)out_cb; (void)out_argc;
    /* This machine makes no request that delivers a value, so nothing below reads the answer to one. Freed on
       every entry, above everything else, because it belongs to no link of the chain. */
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == SD_REMOVE,
           "§6.1.2's deleteRule resumed into a stage the algorithm does not have — its one modelled step is "
           "step 3, and the chain of questions that step may ask is a cursor on this machine's own state "
           "rather than a stage apiece, so a second stage means a resume landed in another algorithm's "
           "numbering");
    /* §6.1.2 declares no `optional` on this member, so a short call is the DECLARATION's to refuse. An equality
       and not a `>=`, so the day this member's IDL grows a position the assert names the line that assumes one. */
    DCHECK(argc == 1,
           "§6.1.2's deleteRule reached its body with an argument count its declaration does not produce — its "
           "one `unsigned long index` is required, so §3.6's argument-count check refuses a shorter call before "
           "this body is entered");
    /* Re-derived on every entry rather than held across the fork: no line between the entry and the ask runs
       the page's code — step_fork_run only clones and re-enters — so re-deriving cannot answer differently,
       and holding the record on a state that PARKS would keep a raw C pointer across a park. The accessor is
       also what captures the sheet into the running flow's COW delta, which a held pointer would skip. */
    s = sheet_here(ctx, hdr->this_val);
    if (!s)
        return JS_STEP_ABRUPT;   /* §3.7.7 Operations' TypeError on a receiver that is not a CSSStyleSheet */
    if (concolic_is(argv[0])) {
        rc = css_rule_delete_index_run(ctx, hdr, c, argv[0], s->rules, &index);
        if (rc)
            return rc;   /* parked at the fork, or §6.4 step 2's IndexSizeError already thrown */
    } else {
        /* THE KNOWN VALUE, THROUGH THE ONE COPY OF THE ARITHMETIC AND ITS ASSERTS. This arm used to spell out
           `idl_number_of` and §3.2.4.6's two postconditions itself, in the same words as the other
           `deleteRule` and as every `item(index)` — one fact written eleven times. It is
           core/idl_index_arg.h's now, beside the chain that answers the unknown half. */
        index = idl_index_arg_known(ctx, argv[0], CSS_RULE_REMOVE_INDEX_ALGORITHM);
    }
    out = css_rule_list_delete(ctx, s->rules, index);
    if (JS_IsException(out))
        return JS_STEP_ABRUPT;
    *presult = out;
    return JS_STEP_DONE;
}

/* The last two are STATED rather than left to the initializer, because both are declarations and not padding:
   `catches_abrupt` 0 says this algorithm does NOT handle an abrupt request result itself — it makes no request
   that can deliver one, so the epilogue's handling is the right one — and `unforkable` NULL says this machine
   may ALWAYS be forked, which is the whole of what it exists for. */
static const IdlStepDecl SD_DECL = {
    js_sheet_delete_rule, sizeof(IdlIndexChain), sd_visit, NULL,
    "CSSOM §6.1.2 The CSSStyleSheet Interface deleteRule(index)", SD_STEPS, 0, NULL
};

/* ---- CSSOM §6.1's CONSTRUCTOR ---------------------------------------------------------------------------- */

/* §6.1.2 The CSSStyleSheet Interface declares the dictionary:
 *
 *     dictionary CSSStyleSheetInit {
 *       DOMString? baseURL = null;
 *       (MediaList or DOMString) media = "";
 *       boolean disabled = false;
 *     };
 *
 * TWO of the three are declared here, in Web IDL §3.2.17 Dictionary types' lexicographic read order — which is
 * `disabled` then `media`, and is stated because the order is part of the declaration and is OBSERVABLE: each
 * member's conversion can run the page's own `toString`, so a dictionary whose rows were sorted any other way
 * would run them out of the order §3.2.17 step 4 specifies. The third is ABSENT, not accepted-and-dropped: an
 * undeclared member is one §3.2.17's conversion never reads, so a page that passes it is in exactly the
 * position of a page talking to a user agent that does not ship the feature, and `node engine/idlgen.mjs`
 * reports it by name every run. Declaring a member this constructor would then ignore is the one thing that
 * would make it invisible.
 *
 * `media` STATES §3.2.15's `I` AS A PREDICATE AND NOT AS A CLASS, which is the whole of what this row needed
 * that no earlier dictionary did. Every indexed interface in this platform is one core/idl_indexed.c object, so
 * `JS_GetClassID` cannot tell a MediaList from a CSSRuleList and `IdlDictMember::iface` — a JSClassID — could
 * not name this interface at all; §4.4 brands on the private-Symbol own slot that HOLDS its collection, and
 * reading an own slot takes a realm. `IdlDictMember::iface_is` is that spelling (core/idl_args.h), and
 * `media_list_is` is already exactly its signature.
 *
 * A NAMED RESIDUAL — `baseURL`. WHAT IS NOT COVERED: `DOMString? baseURL = null` and §6.1's STYLESHEET BASE
 * URL, which step 3 would set from it. WHAT THE NEXT DIFF BUILDS: resolution of a relative `url()` against a
 * sheet's base URL — this build performs none anywhere, for any sheet, so the state item has no reader and a
 * stored one would model nothing a page can observe. HOW ITS ABSENCE SHOWS: the day a `url()` in a constructed
 * sheet's rule is resolved, it resolves against the document rather than against `options.baseURL`. */
static const IdlDictMember CSS_STYLE_SHEET_INIT[] = {
    { "disabled", IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
    /* `(MediaList or DOMString) media = ""` — §3.2.25 Union types over an interface and a string, whose arm IS
       §3.2.15's brand test. The default is the empty string, which §4.4's create a MediaList object turns into
       the empty collection: a sheet that applies to every medium, which is what a page that passed no `media`
       asked for. */
    { "media", IDL_STRING_UNLESS_IFACE, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "",
      .iface_is = media_list_is, .iface_name = "MediaList" },
};

/* §6.1: "CSSStyleSheet(options) — When called, execute the steps to create a constructed CSSStyleSheet given
 * options and return the result." The steps are §6.1's own, and they are numbered here as that list runs.
 *
 * IT IS A PLAIN BODY AND NOT A STEP MACHINE, which is a claim about what runs after the conversion rather than
 * about the algorithm's size: every line below is this engine's own C — a document's base URL, a calloc, an
 * Array — and none of it can reach the page's code, so there is nothing for a machine to suspend at. The one
 * value that could have run a page getter is `options`, and §3.2.17's conversion has already finished with it
 * before this body is entered. */
static JSValue js_css_style_sheet_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    /* `optional CSSStyleSheetInit options = {}` — and the position is CONVERTED even when the page passed no
       argument, because its value is the IDL's `= {}` rather than the page's (core/idl_args.c's §3.6 step 16
       clause, which extends the converted count to cover a declared dictionary position). So `new
       CSSStyleSheet()` reaches this body with a defaults-only dictionary at argv[0], carrying a real `false`
       for `disabled` and a real `""` for `media`, and there is no absent-argument world for a `?:` to fill.
       IT IS ASSERTED RATHER THAN DEFAULTED, which is the difference between reading the IDL's value and
       inventing one: with a `?: JS_UNDEFINED` here the day that clause stopped covering this position would be
       the day every default silently became whatever `undefined` coerces to, and nothing would say so. */
    JSValueConst options;
    const char *base;
    JSValue media, location, obj;
    char *media_text;

    (void)this_val; (void)magic;
    DCHECK(argc > 0, "§6.1.2's `constructor(optional CSSStyleSheetInit options = {})` reached its body with no "
                     "converted argument — a declared dictionary position is converted whether or not the page "
                     "passed one, so this means the conversion stopped short of position 0 and every member's "
                     "IDL default is missing rather than placed");
    options = argv[0];
    /* STEP 2 — "Set sheet's location to the base URL of the associated Document for the current global
       object." THE REALM'S document and not some remembered one: a C member runs in the realm that DEFINED it
       (core/realm.h), so this is the realm whose `CSSStyleSheet` the `new` went through. */
    base = document_base_url(ctx);
    /* A `CHECK` AND NOT A `DCHECK`, BECAUSE THIS LINE IS WHAT DEREFERENCES IT. §2.4.3 Document base URLs gives
       every Document a base URL — the fallback base URL when no `<base href>` names one — so a NULL here is a
       should-never-happen and would ordinarily be a DCHECK. But a DCHECK is compiled out in release and
       `JS_NewString` reads the bytes in BOTH builds, so leaving it one would trade a named dev abort for a
       release read of address zero inside quickjs. `document_base_url` can answer NULL (core/html/html_link.c
       guards its own read), which is why this is asserted at all rather than assumed. */
    CHECK(base != NULL,
          "§6.1's create a constructed CSSStyleSheet asked for the base URL of the associated Document for the "
          "current global object and this realm answered none — §2.4.3 Document base URLs gives every Document "
          "one, an `about:blank` included, so this realm has no document address at all and every URL it builds "
          "would be wrong");
    location = JS_NewString(ctx, base);
    if (JS_IsException(location)) return location;
    /* STEP 12 — "If the media attribute of options is a string, create a MediaList object from the string and
       assign it as sheet's media. Otherwise, serialize a media query list from the attribute and then create a
       MediaList object from the resulting string and set it as sheet's media."
       BOTH ARMS END IN A STRING AND THEREFORE IN §4.4's CREATE, which is why the mint below takes the text and
       not the object: the difference between the two arms is only WHERE the string comes from — the member
       itself, or serialize a media query list (CSSOM §4.2 Serializing Media Queries) over the MediaList the
       page handed over. A page's own MediaList is therefore NOT adopted; the sheet gets a new one carrying the
       same queries, which is what this step (§6.1) says — "create a MediaList object from the resulting
       string" — and what stops two sheets sharing one collection.
       WHICH ARM IS ALREADY DECIDED. `(MediaList or DOMString)` is the declaration's type, so §3.2.25 Union
       types' arm was chosen by the member loop at this member's own position — this body reads the result and
       performs no test of its own. */
    media = idl_dict_get(ctx, options, "media");
    DCHECK(!concolic_is(media),
           "`(MediaList or DOMString) media` reached §6.1's constructor holding UNKNOWN EXTERNAL INPUT. It "
           "crossed as itself (idl_concolic_rule answers CROSSES for the union), and §4.4's create a MediaList "
           "object PARSES its text into a collection of media queries — so there is no way to build the state "
           "item without either inventing bytes the flow never determined or de-tainting the value into a "
           "collection whose serialization a page can read back out of `mediaText`. What is missing is a "
           "MediaList whose collection is unknown: §4.4's members answered over a domain rather than over a "
           "JS Array of serialized queries");
    if (media_list_is(ctx, media)) {
        media_text = media_list_text(ctx, media);   /* §4.2's serialize a media query list */
        CHECK(media_text != NULL, "§6.1 step 12 could not serialize the media query list it was handed");
    } else {
        size_t mlen = 0;
        const char *c = JS_ToCStringLen(ctx, &mlen, media);

        /* The string arm. The value is a real JS string — the union placed it or the member's `= ""` default
           did — so this conversion runs none of the page's code and cannot throw for a reason this body would
           have to model; a NULL is OOM, which is the one thing left. */
        CHECK(c != NULL, "§6.1 step 12 could not read the string arm of `(MediaList or DOMString) media`");
        media_text = malloc(mlen + 1);
        CHECK(media_text != NULL, "cssom: OOM copying a constructed sheet's media text");
        memcpy(media_text, c, mlen);
        media_text[mlen] = '\0';
        JS_FreeCString(ctx, c);
    }
    JS_FreeValue(ctx, media);
    /* STEPS 1 and 4-10, which are the mint: a new object (1); parent CSS style sheet, owner node and owner CSS
       rule all null (4, 5, 6); the title the empty string (7); the alternate flag unset (8) and the
       origin-clean flag set (9), neither of them modelled and both of them what this build already assumes of
       every sheet; the constructed flag set (10). And STEP 13 — "If the disabled attribute of options is true,
       set sheet's disabled flag" — which is the one declared member, read through the declaration's own
       accessor rather than as a post-mint write.
       AND STEP 12's media, whose text was computed above — the mint is where §4.4's create a MediaList object
       runs, because every creator specifies this state item and only this one specifies it from a member.
       STEP 3 ("stylesheet base URL") IS THE ONE RESIDUAL ABOVE.
       STEP 11's `Constructor document` IS NOT STORED, for the reason css_style_sheet.h gives about every state
       item this component does not model: its reader is DOM's `adoptedStyleSheets`, which is absent. */
    obj = sheet_mint(ctx, JS_NULL, JS_NULL, JS_NULL, location, media_text,
                     /*constructed*/ true, idl_dict_bool(ctx, options, "disabled"));
    free(media_text);
    JS_FreeValue(ctx, location);
    /* NO `style_sheet_list_add`, AND THAT IS THE WHOLE DIFFERENCE FROM §6.2's CREATE. A constructed sheet has no
       owner node, so it is in no document's or shadow root's collection; `document.styleSheets` lists the
       top-level sheets of a tree and a constructed one reaches a document only through `adoptedStyleSheets`.
       core/css/style_sheet_list.c's add asserts the same thing from its own side. */
    return obj;   /* STEP 14 — "Return sheet." */
}

/* ---- the interfaces ------------------------------------------------------------------------------------- */

void css_style_sheet_init(JSContext *ctx)
{
    JSClassDef d = { "CSSStyleSheet", sheet_finalizer, sheet_gc_mark };

    if (g_sheet_class) return;   /* one AGENT, one class and one set of pool entries */
    JS_NewClassID(JS_GetRuntime(ctx), &g_sheet_class);
    JS_NewClass(JS_GetRuntime(ctx), g_sheet_class, &d);
    g_stylesheet_proto_slot = realm_value_declare(ctx, "CSSOM §6.1.1 StyleSheet.prototype");
    g_id_set_disabled = idl_setter_id(ctx, IDL_BOOLEAN, false, js_sheet_set_disabled, 0);
    {
        /* §6.1.2: `unsigned long insertRule(CSSOMString rule, optional unsigned long index = 0)` and
           `undefined deleteRule(unsigned long index)`. The optional index and both conversions are the
           declaration's — which is a statement about the CONVERSION and never a licence for a body to run one
           of its own: an `unsigned long` reaches a body either as the Number §3.2.4.6 produced or as unknown
           external input CROSSING that boundary as itself, and this comment used to end "so neither body
           converts anything", under which both bodies converted. */
        static const IdlArgType INSERT[2] = { IDL_DOMSTRING, IDL_UNSIGNED_LONG };
        static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };

        g_id_insert_rule = idl_method_id(ctx, INSERT, 2, js_sheet_insert_rule, 0);
        idl_optional_from(1);
        /* §6.1.2's `deleteRule` IS A MACHINE, and it is a DECLARATION rather than a dispatch: nothing asks at a
           call site which implementation to run, because there is no second body for anything to select
           against. Its one `unsigned long index` can be unknown external input, and asking §6.4's step 2 over
           one needs a state to snapshot. */
        g_id_delete_rule = idl_method_id_step(ctx, ONE_ULONG, 1, NULL, 0, &SD_DECL, 0);
    }
    {
        /* §6.1.2's `constructor(optional CSSStyleSheetInit options = {})`. The dictionary's members are declared
           beside the type, which is what makes §3.2.17's conversion the DECLARATION's work and leaves the body
           with a value to read rather than a getter to run. */
        static const IdlArgType CTOR[1] = { IDL_DICT };

        g_id_ctor = idl_method_id_dict(ctx, CTOR, 1, CSS_STYLE_SHEET_INIT,
                                       (int)(sizeof(CSS_STYLE_SHEET_INIT) / sizeof(CSS_STYLE_SHEET_INIT[0])),
                                       js_css_style_sheet_ctor, 0);
        idl_optional_from(0);   /* `optional CSSStyleSheetInit options = {}` */
    }
    realm_declare_intrinsic(css_style_sheet_install_proto);
}

void css_style_sheet_install_proto(JSContext *ctx)
{
    JSValue base, proto, prev;

    DCHECK(g_sheet_class != 0, "a realm asked for the style-sheet prototypes before the interfaces existed");
    prev = JS_GetClassProto(ctx, g_sheet_class);
    DCHECK(JS_IsNull(prev), "css_style_sheet_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* §6.1.1's StyleSheet.prototype. It carries the six members StyleSheet declares, and CSSStyleSheet.prototype
       INHERITS them — which is what `CSSStyleSheet : StyleSheet` means and is why they are not copied onto both.
       It holds no class of its own because nothing is ever an instance of it. */
    base = JS_NewObject(ctx);
    CHECK(!JS_IsException(base), "StyleSheet.prototype could not be allocated");
    idl_interface_tag(ctx, base, "StyleSheet");
    idl_install_accessor(ctx, base, "type", js_sheet_get, SS_TYPE, -1);
    idl_install_accessor(ctx, base, "href", js_sheet_get, SS_HREF, -1);
    idl_install_accessor(ctx, base, "ownerNode", js_sheet_get, SS_OWNER_NODE, -1);
    idl_install_accessor(ctx, base, "parentStyleSheet", js_sheet_get, SS_PARENT_STYLE_SHEET, -1);
    idl_install_accessor(ctx, base, "title", js_sheet_get, SS_TITLE, -1);
    /* `[SameObject, PutForwards=mediaText] readonly attribute MediaList media`. The setter is NOT this
       component's: Web IDL §3.3.10 [PutForwards] is a BINDING rule, and its five steps are the same five for
       every `media`/`mediaText` pair in the platform — CSSMediaRule's and CSSImportRule's carriers install the
       very same declaration (core/css/media_list.c owns it, because the PAIR is what §4.4 owns). So
       `sheet.media = "print"` is a real [[Get]] of `media` followed by a [[Set]] of `mediaText` on what it got,
       which is why the getter above must answer the state item itself and never a copy. */
    idl_install_accessor(ctx, base, "media", js_sheet_get, SS_MEDIA, media_list_put_forwards_setter());
    idl_install_accessor(ctx, base, "disabled", js_sheet_get, SS_DISABLED, g_id_set_disabled);

    proto = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(proto), "CSSStyleSheet.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CSSStyleSheet");
    idl_install_accessor(ctx, proto, "ownerRule", js_sheet_get, SS_OWNER_RULE, -1);
    idl_install_accessor(ctx, proto, "cssRules", js_sheet_css_rules, 0, -1);
    idl_install_method(ctx, proto, "insertRule", g_id_insert_rule);
    idl_install_method(ctx, proto, "deleteRule", g_id_delete_rule);
    JS_SetClassProto(ctx, g_sheet_class, proto);
    realm_value_set(ctx, g_stylesheet_proto_slot, base);
}

void css_style_sheet_install(JSContext *ctx, JSValueConst global)
{
    JSValue base = realm_value_get(ctx, g_stylesheet_proto_slot);
    JSValue proto = JS_GetClassProto(ctx, g_sheet_class);
    JSValue ctor;

    DCHECK(!JS_IsNull(proto) && JS_IsObject(base),
           "the style-sheet interfaces were installed in a realm that never ran their prototype install");
    /* §6.1.1 declares NO constructor — StyleSheet is "an abstract, base style sheet" and nothing instantiates
       one — so its interface object exists to be what `instanceof` names and nothing else. */
    JS_SetPropertyStr(ctx, (JSValue)global, "StyleSheet", idl_interface_object(ctx, "StyleSheet", base));
    /* §6.1.2's `constructor(optional CSSStyleSheetInit options = {})`, whose steps are §6.1's create a
       constructed CSSStyleSheet. Web IDL §3.7.1 Interface object's [[Construct]] is the DECLARATION's, minted
       here, so `new CSSStyleSheet()` no longer takes the shared "Illegal constructor" TypeError. */
    DCHECK(g_id_ctor >= 0, "CSSStyleSheet was installed before its constructor was declared");
    ctor = idl_step_constructor(ctx, "CSSStyleSheet", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the CSSStyleSheet interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "CSSStyleSheet", ctor);
    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, proto);
}

void css_style_sheet_free(JSRuntime *rt)
{
    (void)rt;   /* both prototypes are the REALM's — released with its context, and the class is the agent's */
}
