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
#include "core/css/css_style_sheet.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/realm.h"
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
    bool    disabled;            /* §6.1 "disabled flag" */
} CssStyleSheetData;

static JSClassID g_sheet_class;
static int       g_stylesheet_proto_slot = -1;   /* StyleSheet.prototype, per realm */
static int       g_id_set_disabled = -1;

/* WHAT THE RECORD OWNS. One list, three readers: the COW layout below, the finalizer, and the gc_mark. */
static const uint16_t SHEET_VALS[] = {
    (uint16_t)offsetof(CssStyleSheetData, owner_node),
    (uint16_t)offsetof(CssStyleSheetData, parent_style_sheet),
    (uint16_t)offsetof(CssStyleSheetData, owner_rule),
    (uint16_t)offsetof(CssStyleSheetData, location),
    (uint16_t)offsetof(CssStyleSheetData, title),
};
static const CowRecord SHEET_REC = { sizeof(CssStyleSheetData), SHEET_VALS, 5 };

/* THE ACCESSOR, AND THEREFORE THE CAPTURE POINT. Every member of both interfaces reaches the record through
   here, so every flow that can write the disabled flag has already had its delta take a copy of the record. */
static CssStyleSheetData *sheet_of(JSValueConst v)
{
    CssStyleSheetData *s = JS_GetOpaque(v, g_sheet_class);

    if (s) cow_capture_host_record(v, s, &SHEET_REC);
    return s;
}

/* The receiver, brand-checked. Both interfaces declare every member on a PROTOTYPE, so a page can apply one to
   anything at all and §3.7.5's answer is a TypeError rather than a read of nothing. */
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

enum { SS_TYPE = 0, SS_HREF, SS_OWNER_NODE, SS_PARENT_STYLE_SHEET, SS_TITLE, SS_DISABLED, SS_OWNER_RULE };

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

/* ---- §6.1's "create a CSS style sheet" and §6.2's "remove a CSS style sheet" ------------------------------ */

JSValue css_style_sheet_create(JSContext *ctx, JSValueConst owner_node, JSValueConst parent_style_sheet,
                               JSValueConst owner_rule, JSValueConst location)
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
    /* §6.1: the disabled flag is "unset by default" and no creator specifies it — the calloc IS that sentence,
       and this asserts it rather than leaving the reader to work out that a zeroed bool means unset. */
    DCHECK(!s->disabled, "a newly created CSS style sheet came out with its disabled flag already set");
    JS_SetOpaque(obj, s);
    /* §6.1's create step 2 — "run the add a CSS style sheet steps for the newly created CSS style sheet" —
       adds it to the document's list. The day that list exists, this creator has to run those steps. */
    realm_awaits(ctx, "Document.prototype.styleSheets",
                 "CSSOM §6.1's create-a-CSS-style-sheet step 2, \"run the add a CSS style sheet steps for the "
                 "newly created CSS style sheet\" — §6.2's add adds it to the list of document or shadow root "
                 "CSS style sheets at the appropriate location and then decides its disabled flag from the "
                 "preferred CSS style sheet set name. Neither the list nor the set names exist yet; the moment "
                 "`styleSheets` does, both halves belong here");
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
    /* §6.2's remove step 1 is "remove the CSS style sheet from the list of document or shadow root CSS style
       sheets", and there is no such list to remove it from. */
    realm_awaits(ctx, "Document.prototype.styleSheets",
                 "CSSOM §6.2's remove-a-CSS-style-sheet step 1, \"remove the CSS style sheet from the list of "
                 "document or shadow root CSS style sheets\" — only step 2 runs while there is no list, so a "
                 "removed sheet would stay in `styleSheets` forever the moment that collection lands");
    /* THE TITLE IS FROZEN BEFORE THE OWNER NODE GOES, which is the whole of why the record carries one: §6.2
       nulls the owner node and says nothing about the title, so after this line there is nothing left to read
       it off and the last value the live read produced IS the title from here on. */
    title = sheet_title_concept(ctx, s);
    JS_FreeValue(ctx, s->title);
    s->title = title;
    /* Step 2. */
    JS_FreeValue(ctx, s->owner_node);
    JS_FreeValue(ctx, s->parent_style_sheet);
    JS_FreeValue(ctx, s->owner_rule);
    s->owner_node = JS_NULL;
    s->parent_style_sheet = JS_NULL;
    s->owner_rule = JS_NULL;
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
    idl_install_accessor(ctx, base, "disabled", js_sheet_get, SS_DISABLED, g_id_set_disabled);

    proto = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(proto), "CSSStyleSheet.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CSSStyleSheet");
    idl_install_accessor(ctx, proto, "ownerRule", js_sheet_get, SS_OWNER_RULE, -1);
    JS_SetClassProto(ctx, g_sheet_class, proto);
    realm_value_set(ctx, g_stylesheet_proto_slot, base);
}

void css_style_sheet_install(JSContext *ctx, JSValueConst global)
{
    JSValue base = realm_value_get(ctx, g_stylesheet_proto_slot);
    JSValue proto = JS_GetClassProto(ctx, g_sheet_class);

    DCHECK(!JS_IsNull(proto) && JS_IsObject(base),
           "the style-sheet interfaces were installed in a realm that never ran their prototype install");
    JS_SetPropertyStr(ctx, (JSValue)global, "StyleSheet", idl_interface_object(ctx, "StyleSheet", base));
    /* §6.1.2 DECLARES A CONSTRUCTOR — `constructor(optional CSSStyleSheetInit options = {})` — and this engine
       has none, so what a page gets from `new CSSStyleSheet()` is the interface object's TypeError. That is the
       honest absence rather than a sheet with no rules: a constructed sheet is a real state machine (the
       constructed flag, the constructor document, the base URL, `replace`/`replaceSync` and the disallow
       modification flag they set), and every one of those is read by members that do not exist either. */
    JS_SetPropertyStr(ctx, (JSValue)global, "CSSStyleSheet", idl_interface_object(ctx, "CSSStyleSheet", proto));
    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, proto);
}

void css_style_sheet_free(JSContext *ctx)
{
    (void)ctx;   /* both prototypes are the REALM's — released with its context, and the class is the agent's */
}
