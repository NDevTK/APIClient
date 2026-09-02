/* HTML §3.1.7 "DOM tree accessors"'s `currentScript`, and §4.12.1.1 "Processing model"'s bracket around a
 * classic script — see document_current_script.h for why the bracket is a per-flow slot and not a C save/restore
 * around the call that starts the program. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/dom/document.h"
#include "core/dom/document_current_script.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/loader/document_scripts.h"   /* §4.12.1's type-string steps and its LAST STEPS, over the element in this slot */
#include "core/idl_args.h"
#include "core/realm.h"

/* THE PER-REALM SLOT §3.1.7's value lives in — a record built with the realm, holding one field. It is the
   shape §3.1.5's readiness uses and it is chosen for the same two reasons: the record is unreachable from the
   page, so nothing can write `currentScript` but §4.12.1.1; and the field is an ordinary property write, so the
   heap COW delta captures it and two script flows over one document never see each other's. */
static int g_cs_slot = -1;

/* THE ONE FIELD NAME, spelled once — a name read in one place and written in another is the broken contract
   CLAUDE.md's §Offensive-programming rule is about, and two string literals is how it becomes one. */
#define CS_FIELD "el"

/* THIS REALM'S RECORD. OWNED — the caller frees. */
static JSValue cs_record(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_cs_slot);

    DCHECK(JS_IsObject(rec),
           "a realm answered for its §3.1.7 currentScript with no record — the record is built by "
           "document_current_script_install, which every realm reaches through core/realm.h's one list, so a "
           "realm without one is a realm that was built somewhere that list does not run");
    return rec;
}

/* WHAT THE SLOT HOLDS RIGHT NOW, in `ctx`'s realm. OWNED. */
static JSValue cs_get(JSContext *ctx)
{
    JSValue rec = cs_record(ctx), v;

    v = JS_GetPropertyStr(ctx, rec, CS_FIELD);
    JS_FreeValue(ctx, rec);
    return v;
}

/* …and the write. `v` is CONSUMED. This is the line the COW delta captures. */
static void cs_set(JSContext *ctx, JSValue v)
{
    JSValue rec = cs_record(ctx);

    JS_SetPropertyStr(ctx, rec, CS_FIELD, v);
    JS_FreeValue(ctx, rec);
}

/* HTML §4.12.1.1 "Processing model"'s "If el's FROM AN EXTERNAL FILE is true, or el's type is 'module', then
 * increment document's ignore-destructive-writes counter", asked of the element the SAME algorithm recorded.
 *
 * WHY THE QUESTION IS ASKED HERE AND NOT COUNTED. §8.4.3's counter is what stops an external script from
 * blowing the document away by implicitly calling `document.open()` — the standard says so where it declares
 * it — and its producer is the bracket around "execute the script element", which in this engine is a WORK
 * ITEM (solver/engine.c compiles the program and the flow runs it across scheduler steps). A counter raised by
 * a C bracket around that would be the wrong flow's, which is the defect this file's header records for the
 * slot itself. So the CLASSIC arm's condition is read off the element §4.12.1.1 already parked here for the
 * duration of the same program: `currentScript` is that element for a classic script and is restored to null
 * the moment it finishes, so a timer callback or an event handler scheduled by it answers false.
 * WHAT IT DOES NOT COVER IS THE MODULE ARM, and that is not a hole this can close: §4.12.1.1 asserts
 * `currentScript` is NULL while a module runs, so a module and an event handler are the same answer here. The
 * module half needs the counter's real producer, at the site that creates the program's frame. */
bool document_current_script_is_from_external_file(JSContext *ctx)
{
    JSValue v = cs_get(ctx);
    lxb_dom_node_t *n = node_of(v);
    bool external = false;

    /* "FROM AN EXTERNAL FILE" is set when the script's source text came from a fetch, which for an element
       that is EXECUTING is exactly its having a `src`: §4.12.1's own steps take the src branch on the
       ATTRIBUTE's presence, and an element whose src did not fetch never reaches execution at all. */
    if (n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT)
        external = lxb_dom_element_has_attribute(lxb_dom_interface_element(n),
                                                 (const lxb_char_t *)"src", 3);
    JS_FreeValue(ctx, v);
    return external;
}

/* HTML §8.4.1 "Opening the input stream" step 5's "if document has an ACTIVE PARSER WHOSE SCRIPT NESTING LEVEL
 * IS GREATER THAN 0", asked of the element §4.12.1.1 parked here — which is the only place this engine records
 * which of a document's scripts is running.
 *
 * THE QUESTION IS ABOUT THE SCHEDULE AND NOT ABOUT THE READINESS, and getting that wrong is a crash in the
 * wrong place. "The parser is standing inside this script" is true for exactly two of §4.12.1's five
 * destinations — an inline classic script and the pending parsing-blocking script — because those are the two
 * §13.2.6.4.8 'The "text" insertion mode' runs WITH the insertion point defined and the tokenizer stopped. A
 * `defer` script and a module run from §13.2.7 "The end" step 5, after the parse; the two `as soon as
 * possible` destinations are not parser-inserted at all. All three of those are destructive writes in a
 * browser, so answering them with the readiness — which is still "loading" for every one of them — would
 * refuse a write the standard performs. So would the readiness alone for a write made by ANOTHER document's
 * script into this one (`w = open(url); w.document.open()` while the popup is still loading), which the corpus
 * does and for which the answer here is null: no script of THIS document is running.
 * A MODULE ARM IS ABSENT ON PURPOSE. §4.12.1.1 asserts this slot is null while a module runs, and a module is
 * never parser-executed, so "null" and "not parser-executed" are the same answer for it. */
bool document_current_script_is_parser_executed(JSContext *ctx)
{
    JSValue v = cs_get(ctx);
    lxb_dom_node_t *n = node_of(v);
    bool parser_executed = false;

    if (n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        lxb_dom_element_t *el = lxb_dom_interface_element(n);
        ScriptType ty = script_block_type(el);

        /* PARSER-INSERTED WITH `force async` FALSE are facts about the element in this slot rather than
           defaults: §4.12.1.1's classic arm is what wrote it, and core/loader/document_scripts.c states the
           same pair for the parse scan that collects these elements. An element a SCRIPT inserted reaches
           §4.12.1 through core/html/html_script.c, where neither holds — and a document whose own script is
           not running has a null slot, which never gets here. */
        if (script_type_executes(ty)) {
            ScriptSchedule sc = script_block_schedule(el, ty, /*parser_inserted*/true, /*force_async*/false);

            parser_executed = sc == SCRIPT_SCHED_IMMEDIATE || sc == SCRIPT_SCHED_PARSER_BLOCKING;
        }
    }
    JS_FreeValue(ctx, v);
    return parser_executed;
}

bool document_current_script_is_null(JSContext *ctx)
{
    JSValue v = cs_get(ctx);
    bool null = JS_IsNull(v);

    JS_FreeValue(ctx, v);
    return null;
}

JSValue document_current_script_set(JSContext *ctx, lxb_dom_element_t *el)
{
    lxb_dom_node_t *n;
    JSValue prev;

    DCHECK(el != NULL, "§4.12.1.1's classic arm was entered with no element — the arm is a switch on EL's type, "
                       "so a program with no `script` element behind it (a `setTimeout` string, a lazy chunk's "
                       "reply, a `javascript:` URL, an @S candidate) never reaches it and leaves the slot null, "
                       "which is §3.1.7's own answer for a document that is not executing a script element");
    n = lxb_dom_interface_node(el);
    /* STEP 1: "Let oldCurrentScript be the value to which document's currentScript object was most recently
       set." IT IS RETURNED AND NOT ASSERTED NULL — see the header: §4.12.1.1 step 36 runs a program while the
       program that inserted the element is mid-statement, so a nested run's old value is the causing script's
       element and there is nothing here to prove. The caller whose premise still holds asserts it on what it
       is handed back, which is where the premise can be stated. */
    prev = cs_get(ctx);
    /* STEP 2: "If el's root is not a shadow root, then set document's currentScript attribute to el. Otherwise,
       set it to null."
       AND THE STANDARD'S OWN NOTE ON WHY IT IS THE ROOT AND NOT THE TREE: "This does not use the in a document
       tree check, as el could have been removed from the document prior to execution, and in that scenario
       currentScript still needs to point to it." So a page that inserts a `<script>` and removes it again before
       the program runs still reads the element back, and nothing here may test connectedness. */
    cs_set(ctx, shadow_root_is(node_root(n)) ? JS_NULL : node_wrap(ctx, n));
    return prev;
}

void document_current_script_restore(JSContext *ctx, lxb_dom_element_t *el, JSValue old)
{
    lxb_dom_node_t *n;
    JSValue cur;

    DCHECK(el != NULL, "§4.12.1.1's classic arm step 4 was reached for a program with no `script` element — "
                       "the restore is asked of exactly the rows the set was, so a caller reaching one without "
                       "the other has lost which program it is completing");
    n = lxb_dom_interface_node(el);
    cur = cs_get(ctx);
    /* THE TWO-SIDED HALF OF THE SET'S ASSERTION, and the one that catches an interleave rather than a skip: what
       this program is clearing must be what THIS program wrote. Step 2's condition is evaluated ONCE, at the
       set, and the page may move the element between the two — so both of the values step 2 can write are
       accepted here and nothing else is. An element that is neither is another flow's, read through a delta
       that did not isolate the write. */
    DCHECK(JS_IsNull(cur) || JS_VALUE_GET_PTR(cur) == JS_VALUE_GET_PTR(node_wrap_peek(n)),
           "a classic script's completion found some OTHER element in this document's currentScript — the slot "
           "is written by §4.12.1.1's classic arm and cleared by its step 4, and one program may not observe "
           "another's element there: two flows' writes have leaked across the COW delta, or a program was "
           "started without its restore having run");
    JS_FreeValue(ctx, cur);
    /* STEP 4: "Set document's currentScript attribute to oldCurrentScript." */
    cs_set(ctx, old);
}

/* §3.1.7's GETTER, whose whole text is "return the value to which it was most recently set". */
static JSValue js_doc_current_script(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    JSContext *realm;
    JSValue rec, v;

    (void)magic;
    /* WEB IDL §3.7.6 Attributes' BRAND CHECK — a TypeError thrown AT THE READ, not a DCHECK: the corpus pulls
       these
       getters off the prototype and applies them to strangers deliberately. */
    if (!n || n->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_ThrowTypeError(ctx, "this is not a Document");
    /* THE VALUE IS THE RECEIVER'S DOCUMENT'S, and the realm is asked for it the way `readyState` asks. A
       Document that is no realm's ACTIVE document — `createHTMLDocument`, a DOMParser parse, `responseXML` —
       has no browsing context and therefore never executes a script element, so nothing has ever set its slot
       and §3.1.7's initialization value IS the answer. */
    realm = document_active_realm_of(n);
    if (!realm) return JS_NULL;
    rec = realm_value_get(realm, g_cs_slot);
    DCHECK(JS_IsObject(rec), "a realm answered for its §3.1.7 currentScript with no record");
    v = JS_GetPropertyStr(ctx, rec, CS_FIELD);
    JS_FreeValue(ctx, rec);
    return v;
}

void document_current_script_init(JSContext *ctx)
{
    DCHECK(g_cs_slot < 0, "document_current_script_init ran twice — the slot is declared once per AGENT and the "
                          "record is built once per REALM");
    g_cs_slot = realm_value_declare(ctx, "HTML §3.1.7 currentScript");
    /* DECLARED UNDER `document`, because that is the row of core/platform.c's one list this component is
       released from: document_init declares it and document_agent_free gives it back, exactly as §3.1.5's
       readiness slot beside it. */
    agent_state_id("document", &g_cs_slot, "the per-realm slot HTML §3.1.7's currentScript lives in");
}

void document_current_script_install(JSContext *ctx, JSValueConst proto)
{
    JSValue rec;

    DCHECK(g_cs_slot >= 0, "§3.1.7's currentScript was installed into a realm before it was declared — the "
                           "declaration is the AGENT's and the install is the REALM's");
    /* §3.1.1: `readonly attribute HTMLOrSVGScriptElement? currentScript` — no setter, so the setter id is -1
       and an assignment is a TypeError in strict mode rather than a slot the page can forge. */
    idl_install_accessor(ctx, proto, "currentScript", js_doc_current_script, 0, -1);
    /* §3.1.7: "When the Document is created, the currentScript must be initialized to null." BUILT WITH THE
       REALM so it belongs to the pre-boot BASELINE — a record made on first touch would be made inside
       whichever flow happened to read first and would then be that flow's, not the one every flow forks from. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "this realm's §3.1.7 currentScript record could not be allocated");
    JS_SetPropertyStr(ctx, rec, CS_FIELD, JS_NULL);
    realm_value_set(ctx, g_cs_slot, rec);
}

void document_current_script_free(void)
{
    DCHECK(g_cs_slot >= 0, "§3.1.7's currentScript was released in an agent that never declared it");
    /* THE RECORDS ARE THE REALMS' — each is in its own per-context slot and released with its context. What
       this component itself holds is the slot id. */
    g_cs_slot = -1;
}
