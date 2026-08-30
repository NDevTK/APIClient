/* CSSOM §6.2 — CSS style sheet collections. See style_sheet_list.h for where the list lives and why.
 *
 * "AT THE APPROPRIATE LOCATION" IS TREE ORDER, AND THE LIST'S OWN ORDER IS WHAT MAKES FINDING IT O(TREE).
 * The naive reading — for each node in the tree, ask whether any sheet in the list owns it — is O(tree x list)
 * per insertion. It is not needed, because this function is the ONLY thing that ever inserts and it therefore
 * MAINTAINS the invariant it wants to use: the list is already in tree order, so at any point in the walk the
 * only sheet whose owner could be the node under the cursor is the one at the running count. One list read per
 * node, and the invariant is asserted rather than assumed.
 *
 * THE INDEXED VIEW IS LIVE BECAUSE IT SHARES THE ARRAY OBJECT. §6.1.2's own note about `cssRules` — "even
 * though the returned object is read-only it can nevertheless change over time due to its liveness status" —
 * is the same requirement one level up: `document.styleSheets` is [SameObject], so a page holds ONE
 * StyleSheetList across every mutation, and it must see sheets added after it was minted. The collection holds
 * the very Array the root's wrapper holds, and add/remove mutate that Array in place. Rebuilding a fresh Array
 * on each mutation would leave every StyleSheetList ever handed out pointing at a snapshot. */
#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_style_sheet.h"
#include "core/css/style_sheet_list.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/idl_args.h"
#include "core/idl_indexed.h"
#include "core/realm.h"
#include "solver/concolic.h"

static JSClassID g_list_class;
/* THE FOUR PRIVATE KEYS, all Symbols so a page can neither see nor forge one, and all read as own SLOTS.
   `g_sheets_key` hangs §6.2's Array off the ROOT's wrapper; `g_view_key` hangs the [SameObject] collection off
   that same wrapper; `g_holder_key` hangs the root's wrapper off the SHEET, which is how a removal finds the
   list it was added to; and `g_backing_key` hangs the very same Array off the COLLECTION.
   THE LAST TWO OF THOSE ARE TWO KEYS FOR ONE ARRAY ON PURPOSE. The collection's slot is also its BRAND — an
   indexed-property object is what anything with an indexed getter is, so the own slot is the only thing that
   tells one collection from another — and the root's wrapper carries the array as well. One key for both would
   have made `StyleSheetList.prototype.length.call(document)` answer instead of throwing, because a Document
   would have passed the brand check on the strength of holding its own list. */
static JSValue g_sheets_key = JS_UNDEFINED, g_view_key = JS_UNDEFINED, g_holder_key = JS_UNDEFINED,
               g_backing_key = JS_UNDEFINED;
static JSAtom  g_atom_sheets = JS_ATOM_NULL, g_atom_view = JS_ATOM_NULL, g_atom_holder = JS_ATOM_NULL,
               g_atom_backing = JS_ATOM_NULL;
static int     g_id_item = -1;

/* ---- INFRA's list operations over a JS Array ------------------------------------------------------------- */

static uint32_t list_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

/* INFRA's "insert at" — opens the gap, which is what "at the appropriate location" needs and what an append
   cannot do: a `<style>` prepended to the head belongs at index 0 however late the page wrote it. */
static void list_insert_at(JSContext *ctx, JSValueConst list, uint32_t i, JSValue v)   /* CONSUMES v */
{
    uint32_t n = list_len(ctx, list), k;

    DCHECK(i <= n, "§6.2 inserted a CSS style sheet at an index past the end of the list");
    for (k = n; k > i; k--)
        JS_SetPropertyUint32(ctx, (JSValue)list, k, JS_GetPropertyUint32(ctx, list, k - 1));
    JS_SetPropertyUint32(ctx, (JSValue)list, i, v);
}

/* INFRA's "remove", which CLOSES the gap — the list is dense and its indices ARE §6.2.2's supported property
   indices, so a hole would be a `styleSheets[i]` of undefined between two real sheets. */
static void list_remove_at(JSContext *ctx, JSValueConst list, uint32_t i)
{
    uint32_t n = list_len(ctx, list), k;

    DCHECK(i < n, "§6.2 removed a CSS style sheet at an index the list does not have");
    for (k = i + 1; k < n; k++)
        JS_SetPropertyUint32(ctx, (JSValue)list, k - 1, JS_GetPropertyUint32(ctx, list, k));
    JS_SetPropertyStr(ctx, (JSValue)list, "length", JS_NewUint32(ctx, n - 1));
}

/* Membership by OBJECT IDENTITY: §6.1's create mints exactly one object per sheet, so the sheet a removal holds
   is pointer-identical to the one the add put in. */
static int64_t list_index_of(JSContext *ctx, JSValueConst list, JSValueConst v)
{
    uint32_t n = list_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(v);

        JS_FreeValue(ctx, e);
        if (same) return (int64_t)i;
    }
    return -1;
}

/* ---- the list a root keeps ------------------------------------------------------------------------------- */

/* The Array of `root_wrap`'s CSS style sheets, CREATING it when the root has none. Creating it here rather than
   at the root's construction is what keeps a document that declares no styles from carrying one; the array is
   made by whichever flow first needs it, and a flow that made it owns it (the delta skips a flow's own
   creations, which is correct — no other flow can see this one's array). OWNED. */
static JSValue root_sheets(JSContext *ctx, JSValueConst root_wrap)
{
    JSValue cur;

    DCHECK(g_atom_sheets != JS_ATOM_NULL, "§6.2's list was asked for before style_sheet_list_init ran");
    if (JS_GetOwnSlot(ctx, &cur, root_wrap, g_atom_sheets) > 0 && JS_IsArray(cur)) return cur;
    JS_FreeValue(ctx, cur);
    cur = JS_NewArray(ctx);
    CHECK(!JS_IsException(cur), "§6.2's list of document or shadow root CSS style sheets could not be allocated");
    JS_SetProperty(ctx, (JSValue)root_wrap, g_atom_sheets, JS_DupValue(ctx, cur));
    return cur;
}

/* The WRAPPER of `owner`'s root — the Document or the ShadowRoot whose collection the sheet belongs to. OWNED. */
static JSValue root_wrap_of(JSContext *ctx, lxb_dom_node_t *owner)
{
    lxb_dom_node_t *root = node_root(owner);
    JSValue w;

    DCHECK(root->type == LXB_DOM_NODE_TYPE_DOCUMENT || shadow_root_is(root),
           "§6.2's list belongs to a DOCUMENT or a SHADOW ROOT, and an owner node's root is neither — HTML "
           "§4.2.6 step 3 returns for an element that is not connected, so an element that reached the add is "
           "one whose shadow-including root is a document and whose own root is therefore one of the two");
    w = node_wrap(ctx, root);
    DCHECK(JS_IsObject(w), "§6.2's list could not reach the wrapper of the root that holds it");
    return w;
}

/* §6.2's "at the appropriate location" — TREE ORDER over the owner nodes. Returns the index `owner`'s sheet
   belongs at, using the order the list is already in (see the file header). */
static uint32_t appropriate_location(JSContext *ctx, JSValueConst list, lxb_dom_node_t *root,
                                     lxb_dom_node_t *owner)
{
    uint32_t n = list_len(ctx, list), at = 0;
    lxb_dom_node_t *p;

    for (p = root; p; p = node_next_in(p, root)) {
        JSValue s;
        lxb_dom_node_t *o;

        if (p == owner) return at;
        if (at >= n) continue;   /* past every sheet already listed; only `owner` is still being looked for */
        s = JS_GetPropertyUint32(ctx, list, at);
        o = css_style_sheet_owner_node(s);
        JS_FreeValue(ctx, s);
        DCHECK(o != NULL,
               "§6.2's list holds a CSS style sheet with no owner node — the removal that nulls one takes the "
               "sheet out of the list first, so a listed sheet always has the node its position is decided by");
        if (p == o) at++;
    }
    DFAIL("§6.2's add walked the whole of the root's tree without reaching the owner node it was placing — the "
          "root was taken FROM that node, so the walk that starts there must reach it");
    return at;
}

void style_sheet_list_add(JSContext *ctx, JSValueConst sheet, JSValueConst owner_node)
{
    lxb_dom_node_t *owner = node_of(owner_node);
    JSValue root_wrap, list;

    DCHECK(css_style_sheet_is(sheet), "§6.2's add was given something that is not a CSS style sheet");
    /* §6.1's create is the only caller, and every sheet it can build today has an owner node. An @import'd
       sheet has a parent CSS style sheet and an owner CSS rule and NO owner node, and it is also not in the
       document's collection at all — `document.styleSheets` lists the top-level sheets only. So the day
       CSSImportRule creates one, this is the line that has to decide, rather than silently rooting an
       @import'd sheet at whatever tree its parent's owner happens to be in. */
    DCHECK(owner != NULL,
           "§6.2's add a CSS style sheet was given a sheet with NO OWNER NODE. The only such sheet is one an "
           "@import at-rule pulled in (CSSOM §6.4.4's CSSImportRule) or one `new CSSStyleSheet()` constructed, "
           "and NEITHER belongs in the list of document CSS style sheets — `document.styleSheets` holds the "
           "top-level sheets, and a constructed sheet reaches a document only through adoptedStyleSheets. "
           "Whichever of the two lands first must not route through this add");
    root_wrap = root_wrap_of(ctx, owner);
    list = root_sheets(ctx, root_wrap);
    DCHECK(list_index_of(ctx, list, sheet) < 0,
           "§6.2 added a CSS style sheet that is already in the list — a sheet is created once and removed "
           "before it is replaced, so a second add means a removal was skipped");
    list_insert_at(ctx, list, appropriate_location(ctx, list, node_root(owner), owner),
                   JS_DupValue(ctx, sheet));
    /* WHICH LIST, RECORDED — see the header. It is a slot on the SHEET, so it forks and parks with everything
       else, and it is the only thing a removal after the element has left the tree can trust. */
    JS_SetProperty(ctx, (JSValue)sheet, g_atom_holder, root_wrap);   /* CONSUMES root_wrap */
    JS_FreeValue(ctx, list);

    /* STEPS 2-6 ARE A PROVABLE NO-OP FOR EVERY SHEET THIS ENGINE CAN CREATE, AND THIS IS WHERE THAT STOPS
       BEING TRUE. They are their own piece of work and are deliberately not folded in here.
       Step 2 appends the owner node to the document's script-blocking style sheet set when it "contributes a
       script-blocking style sheet" — whose last clause is that the user agent has not given up LOADING the
       sheet. An inline `<style>` has no critical subresources, so it is parsed and processed in the same turn;
       core/html/autofocus.c names `<link rel=stylesheet>` as the element that can be on that set, at the step
       that reads it.
       Steps 3-6 decide the DISABLED FLAG from §6.2's CSS style sheet SETS — the preferred and last set names,
       the alternate flag, and the title they are all matched against. Step 5's first bullet unsets the flag and
       returns for a sheet whose title is the empty string, which is every sheet with no `title` attribute, and
       the flag is already unset. A sheet with a NON-EMPTY title is the one they can act on: step 6 SETS the
       flag for a titled sheet that is not in the enabled set, so skipping them leaves a `<style title="x">`
       that a real browser disables contributing to the cascade. */
    {
        JSValue t = css_style_sheet_title(ctx, sheet);
        size_t tlen = 0;
        const char *c = JS_ToCStringLen(ctx, &tlen, t);

        DCHECK(c != NULL && tlen == 0,
               "CSSOM §6.2's add a CSS style sheet reached a sheet with a NON-EMPTY TITLE, and steps 3-6 are "
               "not built: a titled sheet names a CSS STYLE SHEET SET, and whether its disabled flag ends up "
               "set depends on the document's PREFERRED CSS style sheet set name (§6.2, changed by the first "
               "non-alternate titled sheet and by the HTTP `Default-Style` header) and its LAST CSS style "
               "sheet set name (null until `selectCSSStyleSheetSet`). Give the Document those two names and "
               "the alternate flag, then write steps 3-6 here — until then this sheet is left ENABLED and a "
               "real browser would disable it, so the cascade resolves values the page does not show");
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, t);
    }
}

void style_sheet_list_remove(JSContext *ctx, JSValueConst sheet)
{
    JSValue root_wrap, list;
    int64_t at;

    DCHECK(css_style_sheet_is(sheet), "§6.2's remove was given something that is not a CSS style sheet");
    if (JS_GetOwnSlot(ctx, &root_wrap, sheet, g_atom_holder) <= 0) root_wrap = JS_UNDEFINED;
    DCHECK(JS_IsObject(root_wrap),
           "§6.2's remove a CSS style sheet reached a sheet that no add ever put in a list — §6.1's create runs "
           "the add steps for every sheet it makes, so a sheet with no recorded holder is one built by a path "
           "that skipped them");
    list = root_sheets(ctx, root_wrap);
    at = list_index_of(ctx, list, sheet);
    DCHECK(at >= 0,
           "§6.2's remove a CSS style sheet did not find the sheet in the list its own add recorded — the "
           "holder slot and the list are written together and nothing else writes either");
    list_remove_at(ctx, list, (uint32_t)at);
    /* The holder goes with the membership: a sheet out of every list must not name one, or a second removal
       would read a live index out of a list it is not in. */
    JS_SetProperty(ctx, (JSValue)sheet, g_atom_holder, JS_NULL);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, root_wrap);
}

JSValue style_sheet_list_of(JSContext *ctx, lxb_dom_node_t *root)
{
    JSValue w, cur;

    DCHECK(g_atom_sheets != JS_ATOM_NULL, "§6.2's list was read before style_sheet_list_init ran");
    if (!root || (root->type != LXB_DOM_NODE_TYPE_DOCUMENT && !shadow_root_is(root))) return JS_UNDEFINED;
    w = node_wrap(ctx, root);
    DCHECK(JS_IsObject(w), "§6.2's list could not reach the wrapper of the root that holds it");
    if (JS_GetOwnSlot(ctx, &cur, w, g_atom_sheets) <= 0) cur = JS_UNDEFINED;
    JS_FreeValue(ctx, w);
    if (JS_IsArray(cur)) return cur;
    JS_FreeValue(ctx, cur);
    return JS_UNDEFINED;   /* no `<style>` has ever been placed in this tree — see the header on not minting */
}

/* ---- §6.2.2's StyleSheetList ----------------------------------------------------------------------------- */

/* THE BRAND is the own slot this component put on the object — an indexed-property object is what anything with
   an indexed getter is, so the class alone would not tell one collection from another. Returns JS_UNDEFINED for
   anything that is not a StyleSheetList. */
static JSValue ssl_sheets(JSContext *ctx, JSValueConst v)
{
    JSValue sheets;

    if (!JS_IsObject(v)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &sheets, v, g_atom_backing) <= 0) return JS_UNDEFINED;
    return sheets;
}

/* §6.2.2: "The length attribute must return the number of CSS style sheets represented by the collection." */
static uint32_t ssl_length(JSContext *ctx, JSValueConst self)
{
    JSValue sheets = ssl_sheets(ctx, self);
    uint32_t n = 0;

    if (JS_IsArray(sheets)) n = list_len(ctx, sheets);
    JS_FreeValue(ctx, sheets);
    return n;
}

/* THE INDEXED PROPERTY GETTER — JS_UNDEFINED past the end, which is what a lookup outside §6.2.2's supported
   property indices is. The `item()` operation below turns that into the null its IDL declares. */
static JSValue ssl_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue sheets = ssl_sheets(ctx, self), r;

    if (!JS_IsArray(sheets)) { JS_FreeValue(ctx, sheets); return JS_UNDEFINED; }
    r = JS_GetPropertyUint32(ctx, sheets, i);
    JS_FreeValue(ctx, sheets);
    DCHECK(JS_IsUndefined(r) || css_style_sheet_is(r),
           "a StyleSheetList held something that is not a CSSStyleSheet — its indexed getter and its `item` "
           "both declare `CSSStyleSheet?`, and §6.2's add is the one place anything is ever put in one");
    return r;
}

static const IdlIndexedDecl SSL_INDEXED = { "StyleSheetList", ssl_length, ssl_item, NULL, 0 };

/* Web IDL §3.7.5's brand, asked by the two PROTOTYPE members: the decl callbacks above are reached only through
   an index lookup on an object idl_indexed already resolved, so they answer the empty collection for a
   stranger, while a member read off `StyleSheetList.prototype` directly must THROW — a page tells that apart
   from `undefined`. */
static bool ssl_is(JSContext *ctx, JSValueConst v)
{
    JSValue sheets = ssl_sheets(ctx, v);
    bool ok = JS_IsArray(sheets);

    JS_FreeValue(ctx, sheets);
    return ok;
}

/* §6.2.2's `item(index)`: "must return the indexth CSS style sheet in the collection. If there is no indexth
   object in the collection, then the method must return null." The difference from the indexed getter is
   exactly that null, which is why both exist. */
static JSValue js_ssl_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue r;
    uint32_t i = 0;

    (void)magic;
    if (!ssl_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "StyleSheetList.prototype.item was reached on something that is not a "
                                      "StyleSheetList");
    DCHECK(argc >= 1, "§6.2.2's `item` reached its body with no argument — its IDL argument is required, so the "
                      "declaration's own argument-count check is what should have refused the call");
    if (concolic_is(argv[0])) {
        /* AN UNKNOWN INDEX. The empty collection is the one length at which that has an answer rather than a
           fork: §6.2.2 returns null for every index at or past the length, so a collection of length zero
           answers null over the WHOLE domain and there is no arm to explore. */
        DCHECK(ssl_length(ctx, this_val) == 0,
               "§6.2.2's `item` was given an UNKNOWN index into a NON-EMPTY StyleSheetList — every sheet in it "
               "is a distinct answer, so the read must FORK one flow per supported index (plus the null arm "
               "for an index past the end) instead of deciding it here");
        return JS_NULL;
    }
    JS_ToUint32(ctx, &i, argv[0]);
    r = ssl_item(ctx, this_val, i);
    return JS_IsUndefined(r) ? JS_NULL : r;
}

static JSValue js_ssl_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!ssl_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "StyleSheetList.prototype.length was reached on something that is not a "
                                      "StyleSheetList");
    return JS_NewUint32(ctx, ssl_length(ctx, this_val));
}

/* ---- §6.2.3's `styleSheets` ------------------------------------------------------------------------------ */

/* "The styleSheets attribute must return a StyleSheetList collection representing the document or shadow root
   CSS style sheets." [SameObject], so the collection is remembered on the receiver rather than rebuilt — a page
   holds `document.styleSheets` and compares it, and a fresh object per read makes every such comparison false.
   The slot is on the wrapper, so it is per-flow like everything else there. */
static JSValue js_style_sheets(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue cur, sheets, proto;

    (void)magic;
    DCHECK(g_list_class != 0, "§6.2.3's `styleSheets` ran before style_sheet_list_init declared the interface");
    {
        lxb_dom_node_t *n = node_of(this_val);

        /* Web IDL §3.7.5's brand check, and a THROW rather than an assert: the member is on two prototypes and
           a page reaches an accessor off one with `.call` on anything at all. */
        if (!n || (n->type != LXB_DOM_NODE_TYPE_DOCUMENT && !shadow_root_is(n)))
            return JS_ThrowTypeError(ctx, "styleSheets was reached on something that is neither a Document nor "
                                          "a ShadowRoot");
    }
    if (JS_GetOwnSlot(ctx, &cur, this_val, g_atom_view) > 0 && JS_IsObject(cur)) return cur;
    JS_FreeValue(ctx, cur);

    proto = JS_GetClassProto(ctx, g_list_class);
    DCHECK(!JS_IsNull(proto), "a StyleSheetList was built in a realm that never ran its prototype install");
    cur = idl_indexed_new(ctx, proto, &SSL_INDEXED);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(cur), "a StyleSheetList could not be allocated");
    /* THE VERY ARRAY the root holds, not a copy — that is the whole of the collection's liveness. */
    sheets = root_sheets(ctx, this_val);
    JS_DefinePropertyValue(ctx, cur, g_atom_backing, sheets, 0);
    JS_SetProperty(ctx, (JSValue)this_val, g_atom_view, JS_DupValue(ctx, cur));
    return cur;
}

/* ---- the interface --------------------------------------------------------------------------------------- */

void style_sheet_list_init(JSContext *ctx)
{
    JSClassDef d = { "StyleSheetList" };
    static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };

    if (g_list_class) return;   /* one AGENT, one class and one pool entry */
    JS_NewClassID(JS_GetRuntime(ctx), &g_list_class);
    JS_NewClass(JS_GetRuntime(ctx), g_list_class, &d);
    g_sheets_key = JS_NewSymbol(ctx, "cssStyleSheets", false);
    g_view_key = JS_NewSymbol(ctx, "styleSheetsView", false);
    g_holder_key = JS_NewSymbol(ctx, "cssStyleSheetHolder", false);
    g_backing_key = JS_NewSymbol(ctx, "styleSheetListBacking", false);
    CHECK(!JS_IsException(g_sheets_key) && !JS_IsException(g_view_key) && !JS_IsException(g_holder_key) &&
          !JS_IsException(g_backing_key), "the §6.2 slot key allocations failed");
    g_atom_sheets = JS_ValueToAtom(ctx, g_sheets_key);
    g_atom_view = JS_ValueToAtom(ctx, g_view_key);
    g_atom_holder = JS_ValueToAtom(ctx, g_holder_key);
    g_atom_backing = JS_ValueToAtom(ctx, g_backing_key);
    CHECK(g_atom_sheets != JS_ATOM_NULL && g_atom_view != JS_ATOM_NULL && g_atom_holder != JS_ATOM_NULL &&
          g_atom_backing != JS_ATOM_NULL, "the §6.2 slot keys could not be interned");
    g_id_item = idl_method_id(ctx, ONE_ULONG, 1, js_ssl_item, 0);
    realm_declare_intrinsic(style_sheet_list_install_proto);
}

void style_sheet_list_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_list_class != 0, "a realm asked for StyleSheetList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_list_class);
    DCHECK(JS_IsNull(prev), "style_sheet_list_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "StyleSheetList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "StyleSheetList");
    idl_install_accessor_no_user_code(ctx, proto, "length", js_ssl_length, 0, -1);
    idl_install_method(ctx, proto, "item", 1, g_id_item);
    /* Web IDL §3.7.10: an interface with an indexed property getter and an integer-typed `length` is given
       %Array.prototype.values% as its @@iterator, which is what makes `[...document.styleSheets]` work.
       §6.2.2 declares no `iterable<>`, so it gets that and NOT `entries`/`keys`/`forEach` — two clauses. */
    idl_indexed_install_iterable(ctx, proto);
    JS_SetClassProto(ctx, g_list_class, proto);
}

void style_sheet_list_install_mixin(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_list_class != 0, "§6.2.3's member was installed before style_sheet_list_init ran");
    idl_install_accessor(ctx, proto, "styleSheets", js_style_sheets, 0, -1);
}

void style_sheet_list_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_list_class);

    DCHECK(!JS_IsNull(proto), "StyleSheetList was installed in a realm that never ran its prototype install");
    /* §6.2.2 declares no constructor, so the interface object's call and construct both throw. */
    JS_SetPropertyStr(ctx, (JSValue)global, "StyleSheetList",
                      idl_interface_object(ctx, "StyleSheetList", proto));
    JS_FreeValue(ctx, proto);
}

void style_sheet_list_free(JSRuntime *rt)
{
    if (!g_list_class) return;   /* the prototype is the REALM's — released with its context */
    JS_FreeAtomRT(rt, g_atom_sheets);
    JS_FreeAtomRT(rt, g_atom_view);
    JS_FreeAtomRT(rt, g_atom_holder);
    JS_FreeAtomRT(rt, g_atom_backing);
    g_atom_sheets = g_atom_view = g_atom_holder = g_atom_backing = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_sheets_key);
    JS_FreeValueRT(rt, g_view_key);
    JS_FreeValueRT(rt, g_holder_key);
    JS_FreeValueRT(rt, g_backing_key);
    g_sheets_key = g_view_key = g_holder_key = g_backing_key = JS_UNDEFINED;
    g_id_item = -1;
}
