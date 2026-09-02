/* HTML §8.4 Dynamic markup insertion — see document_write.h for why this is its own component and for the two
   mechanisms `document.write` is waiting on before it can INSERT. What is here is the whole of the algorithm
   that decides WHAT is written, which is the half the @S sink is made of. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/document_current_script.h"   /* §8.4.3 step 9.1's ignore-destructive-writes condition */
#include "core/dom/node.h"
#include "core/html/document_open.h"            /* §8.4.1's document open steps — which §8.4.3 step 9.2 runs
                                                   — and §8.4.2's close(), steps 3-6 */
#include "core/html/document_write.h"
#include "core/html/html_parse.h"
#include "core/html/trusted_types.h"
#include "core/idl_args.h"
#include "solver/concolic.h"
#include "solver/solve.h"

/* §8.4.3 and §8.4.4 are ONE algorithm with two arguments — "run the document write steps with this, text,
   `false`, and 'Document write'" against "… `true`, and 'Document writeln'" — so they are one body with a
   magic and not two that drifted apart. §8.4.2's close() is a different algorithm and has its own. */
enum { DW_WRITE = 0, DW_WRITELN = 1 };

static int g_id_write   = -1;
static int g_id_writeln = -1;
static int g_id_close   = -1;
static int g_id_open    = -1;

/* §8.4.3 step 4 and §8.4.4's own — the standard's SINK NAME, which is what a Trusted Types violation report
   names and what makes two sinks distinguishable in one report. Indexed by the magic above so the pair cannot
   come apart from the `lineFeed` argument they are declared beside. */
static const char *const DW_SINK[2] = { "Document write", "Document writeln" };

/* ---- the receiver -------------------------------------------------------------------------------------- */

/* WEB IDL §3.7.7 Operations' BRAND CHECK — a TypeError thrown AT THE CALL and NOT an engine invariant, for
   the reason
   core/dom/document_domain.c's own receiver states: the corpus pulls these off the prototype and applies them
   to the wrong receiver deliberately, so asserting would turn a test that asks for the throw into an abort. */
static lxb_dom_document_t *dw_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (!n || n->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
        JS_ThrowTypeError(ctx, "this is not a Document");
        return NULL;
    }
    return lxb_dom_interface_document(n);
}

/* DOM §4.5 "Interface Document": "A document is said to be an XML document if its type is 'xml'; otherwise an
   HTML document." This engine states a document's type as its CONTENT TYPE — core/dom/document.c already asks
   exactly this question that way, and `createDocument` mints `application/xml` while every HTML path mints
   `text/html` — so the one test lives here rather than becoming a second statement of the same fact.
   IT IS REACHABLE AND IS NOT A FORMALITY: `document.implementation.createDocument(null, "x").write("<a>")`
   is the throw, and a page's own try/catch can tell it from a write that did nothing. */
static bool dw_is_xml_document(const lxb_dom_document_t *dom)
{
    const char *type = document_content_type_of(dom);

    DCHECK(type != NULL,
           "a Document reached §8.4.3 step 6 with no content type — DOM §4.5 gives every document one at "
           "creation and core/dom/document.c's record is where it is written, so a document without one came "
           "from no creation this engine performs");
    return strcmp(type, "text/html") != 0;
}

/* §8.4.3 STEPS 7 AND 8 ARE DECIDED IN THIS ENGINE, and each is decided by a named absence rather than by a
 * field nobody writes — which is the difference between a positive statement and a default hiding a hole.
 *
 *   STEP 7, the THROW-ON-DYNAMIC-MARKUP-INSERTION COUNTER. §13.2.6.1 "Creating and inserting nodes" is the
 *   ONLY algorithm in HTML that increments it: "create an element for a token" raises it around a custom
 *   element constructor the PARSER runs, and its own note says why — "since we incremented the
 *   throw-on-dynamic-markup-insertion counter, this cannot cause new characters to be inserted into the
 *   tokenizer". This engine's tree construction never runs a constructor (custom elements are upgraded after
 *   the parse), so nothing can raise it and the counter is zero. It becomes a real field in the same diff that
 *   gives this engine a live document parser, because that is the diff in which something first raises it.
 *
 *   STEP 8, ACTIVE PARSER WAS ABORTED. §8.4.1 declares it on the Document and states its initial value —
 *   "Document objects have an active parser was aborted boolean … It is initially false" — and what sets it is
 *   a navigation that aborts a document mid-parse. This engine has no mid-parse state to abort: a document is
 *   parsed to completion before its first flow is seeded. Same rule as step 7 — the field arrives with the
 *   capability that can change it.
 *
 * NEITHER IS WRITTEN AS `if (0)`. A condition over a constant reads as a check and is not one; the statement
 * is the paragraph and the absence of the branch. */

/* ---- §8.4.3 steps 1-3: `string` -------------------------------------------------------------------------- */

/* STEP 3's "append value to string", RUN AS THE REAL OPERATION. §Solver-half: an example propagates because
   the engine runs the op, never because a hook derived a note — so a concolic operand goes through the
   engine's own concatenation (solver/concolic.h's add hook, named as 22.1.3.5's string-concatenation, which
   is what an append to a string is and has no numeric arm to choose), and two concrete operands are
   concatenated for real. `acc` is CONSUMED; the result is owned.
   THE COMMON CALL NEVER REACHES THE CONCATENATION AT ALL. `document.write(x)` is one argument, so `string` IS
   that value and the sink downstream sees the source itself rather than a derivation of it. */
static JSValue dw_append(JSContext *ctx, JSValue acc, JSValueConst v)
{
    JSValue sp[2];
    const char *pa, *pb;
    char *joined;
    size_t la, lb;
    JSValue out;

    if (concolic_is(acc) || concolic_is(v)) {
        sp[0] = acc;                       /* the accumulator's reference moves onto the stack */
        sp[1] = JS_DupValue(ctx, v);
        /* The hook's stack effect is js_add_slow's: both operands freed, the result placed in sp[-2]. */
        if (!concolic_add_hook(ctx, sp + 2, JS_CONCOLIC_ADD_CONCAT))
            DFAIL("§8.4.3 step 3's append declined an operand this component has already established is "
                  "UNKNOWN — the concolic value semantics are not installed in this host, so every operator "
                  "over an unknown falls through to the ordinary-object path and the next coercion throws out "
                  "of an expression the page never wrote (solver/concolic.h: concolic_install_hooks)");
        return sp[0];
    }
    DCHECK(JS_IsString(acc) && JS_IsString(v),
           "§8.4.3 step 3 reached its append with an operand that is neither a String nor unknown external "
           "input — the member's IDL declares `(TrustedHTML or DOMString)... text`, and the declaration is "
           "what converts, so a third kind of value here means the position was not declared");
    pa = JS_ToCString(ctx, acc);
    pb = pa ? JS_ToCString(ctx, v) : NULL;
    CHECK(pa && pb,
          "§8.4.3 step 3's append could not read a String it had already converted — a markup sink that "
          "silently dropped half its bytes would report on a document the page never asked for");
    la = strlen(pa); lb = strlen(pb);
    joined = malloc(la + lb + 1);
    CHECK(joined != NULL, "§8.4.3 step 3's append could not be allocated");
    memcpy(joined, pa, la);
    memcpy(joined + la, pb, lb + 1);
    JS_FreeCString(ctx, pa);
    JS_FreeCString(ctx, pb);
    JS_FreeValue(ctx, acc);
    out = JS_NewString(ctx, joined);
    free(joined);
    return out;
}

/* ---- the machine --------------------------------------------------------------------------------------- */

/* TWO STAGES, AND THE SPLIT IS THE ONE core/html/trusted_types.h ASKS FOR: §3's default policy is the step in
   "get trusted type compliant string" that will run the PAGE's code, and the stage that calls it is where the
   machine has to be able to rest once it can. The second stage is everything the write steps decide about the
   DOCUMENT rather than about the value. */
#define DW_STAGES(X) \
    X(DW_ST_TRUSTED, "HTML §8.4.3 document write steps 1-5 (assemble `string`, the Trusted Types compliant " \
                     "string, and the line feed) — one O(1) engine action per argument") \
    X(DW_ST_WRITE,   "HTML §8.4.3 document write steps 6-11 (the two throws, the aborted-parser return, the " \
                     "insertion point, and handing the string to the parser)")
enum { IDL_STEP_STAGE_BASE(DW_STAGES) DW_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DW_STEPS[] = { DW_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSValue string;   /* §8.4.3's `string` — owned across the stage boundary */
} DwState;

static void dw_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    DwState *s = st;

    v->val(ctx, &s->string);
}

static int dw_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    DwState *s = st;
    const int line_feed = idl_step_magic(hdr) == DW_WRITELN;
    lxb_dom_document_t *dom;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;

    if (hdr->stage == DW_ST_TRUSTED) {
        int i;
        bool is_trusted;

        /* THE STATE IS COMPLETE BEFORE THE FIRST OPERATION THAT CAN THROW — every owned field placed on it,
           because the failure path tears the state down through the declaration's discharge, which frees
           exactly what `dw_visit` names and nothing else. There is one field and it is set here first. */
        s->string = JS_UNDEFINED;
        s->string = JS_NewString(ctx, "");                                        /* STEP 1 */
        if (JS_IsException(s->string)) { s->string = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        /* STEP 2: "let isTrusted be false if text contains a string; otherwise true". §2's TrustedHTML does
           not exist in this engine (core/html/trusted_types.h says so and says why that makes the algorithm
           DECIDED rather than approximate), so every value the declaration produced is a string — which makes
           `isTrusted` false exactly when the page passed at least one argument. THE EMPTY CALL IS NOT A
           FORMALITY: `document.write()` under `require-trusted-types-for 'script'` must NOT throw, and it is
           this step that says so. */
        is_trusted = argc == 0;
        for (i = 0; i < argc; i++)                                                /* STEP 3 */
            s->string = dw_append(ctx, s->string, argv[i]);
        if (!is_trusted) {                                                        /* STEP 4 */
            JSValue compliant = trusted_types_compliant_string(ctx, TRUSTED_TYPE_HTML, s->string,
                                                               DW_SINK[line_feed]);
            if (JS_IsException(compliant)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->string);
            s->string = compliant;
        }
        if (line_feed) {                                                          /* STEP 5 */
            JSValue lf = JS_NewString(ctx, "\n");
            if (JS_IsException(lf)) return JS_STEP_ABRUPT;
            s->string = dw_append(ctx, s->string, lf);
            JS_FreeValue(ctx, lf);
        }
        hdr->stage = DW_ST_WRITE;
    }

    DCHECK(hdr->stage == DW_ST_WRITE, "the document write steps resumed into a stage §8.4.3 does not have");
    if (!(dom = dw_receiver(ctx, hdr->this_val))) return JS_STEP_ABRUPT;
    if (dw_is_xml_document(dom)) {                                                /* STEP 6 */
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "document.write on an XML document");
        return JS_STEP_ABRUPT;
    }
    /* STEPS 7 AND 8 are decided — see the paragraph above dw_append for which absence decides each. */

    /* THE @S MARKUP SINK, ANNOUNCED HERE — the SAME seam core/dom/element.c's innerHTML, outerHTML and
     * insertAdjacentHTML take (solver/solve.h's solve_html_sink), never a second one.
     *
     * WHY IT IS BEFORE STEP 9 AND NOT AFTER THE PARSE. The remaining steps decide nothing about the VALUE;
     * they are questions about this engine's PARSER STATE, and the answers are an artifact of when this engine
     * runs a document's scripts rather than a fact about the page. §8.4.3's own prose says the method's
     * behavior "can in some cases be dependent on network latency" — so a write that the parser state happens
     * to swallow on this run is a write a browser can deliver on the next, and a detector that stayed silent
     * for it would be answering "no sink" because it could not ask. solve.c makes exactly that argument about
     * its own fire oracle ("an oracle may not answer NO because it could not ask"), and §@S makes it about the
     * whole half: absence is never a safe verdict.
     *
     * AND IT IS THE VERIFYING PATH'S ARRIVAL TOO. During a candidate re-run the injected breakout is a real
     * String and this is where its bytes are measured — the filter rung, then the context probe or the fire
     * oracle, all of which solve.c performs on the string itself. Neither half needs this document's tree,
     * which is why the sink lands ahead of the parser rather than behind it. */
    solve_html_sink(ctx, s->string);
    /* NOTHING CONCRETE TO TOKENIZE — core/dom/element.c's own line, and for the same reason: unknown external
       input is not markup, so there is no document to build out of it and the SINK is what this write means.
       This is also the arm `document.write(location.hash)` takes, which is why the canonical DOM-XSS shape
       reaches its search without needing the two mechanisms document_write.h names. */
    if (concolic_is(s->string)) return JS_STEP_DONE;

    DCHECK(JS_IsString(s->string),
           "the document write steps reached step 9 with a `string` that is neither a String nor unknown "
           "external input — steps 1-5 build it out of the declaration's converted arguments, so a third kind "
           "of value means one of those steps produced something §8.4.3 cannot insert into an input stream");
    /* STEP 9 — "if the insertion point is undefined". ASKED THROUGH THE RECORD THAT SAYS WHOSE PARSER IT IS,
       which is §13.2.3.5's own question plus the assertion that this flow and the document's one Lexbor parser
       agree about whether a stream is open; document_open.h states the single case where they do not and why
       it must crash rather than resolve to either answer. */
    if (!document_open_stream_is_ours(ctx, hdr->this_val, dom)) {                 /* STEP 9 */
        /* STEP 9.1 — "If document's unload counter is greater than 0 or document's ignore-destructive-writes
           counter is greater than 0, then return."
           THE SECOND COUNTER IS WHAT KEEPS A REAL PAGE'S DOCUMENT ALIVE, and its absence would not have looked
           like a gap: §8.4.3 declares it "to prevent external scripts from being able to use document.write()
           to blow away the document by implicitly calling document.open()", and a tag manager or an ad slot
           reaching this line from a `<script src>` after the parse has ended is the commonest `document.write`
           on the web. Performed here it would replace the page a browser leaves untouched — silently, and with
           the whole document as the collateral. It is asked of the TARGET document's realm rather than of this
           member's, because step 9.1 is about `document`'s counter and a member pulled off another realm's
           prototype runs in that other realm.
           THE UNLOAD COUNTER IS AN ABSENCE WITH A PRODUCER and is named rather than guessed: §7.5.9 "Unloading
           documents" is the only algorithm that raises it, core/frame/document_lifecycle.c HAS those steps as a
           machine, and the counter has to be raised and lowered by that machine's own stages — a C bracket
           around a work item reads the wrong flow's state (core/dom/document_current_script.h records that
           defect). Until it is there, a `document.write` from a `pagehide` or `unload` handler is performed
           here where a browser ignores it. */
        JSContext *target = document_active_realm_of(lxb_dom_interface_node(dom));

        if (target != NULL && document_current_script_is_from_external_file(target))
            return JS_STEP_DONE;
        /* STEP 9.2 — "Run the document open steps with document." */
        if (!document_open_steps(ctx, hdr->this_val, dom)) return JS_STEP_ABRUPT;
        DCHECK(html_parse_insertion_point_defined(dom),
               "§8.4.3 step 9.2's document open steps returned with no input stream open — every arm of "
               "§8.4.1 that declines to open one RETURNS DOCUMENT at a step this engine has established it "
               "cannot be standing at, so step 10 is about to insert into a stream that is not there");
    }
    {
        /* STEPS 10 AND 11 — one call, because in lexbor the input stream is the tokenizer's own cursor rather
           than a buffer this engine holds (core/html/html_parse.h states it at the entry). */
        const char *text = JS_ToCString(ctx, s->string);
        size_t n;

        CHECK(text != NULL,
              "the document write steps could not read the String they had already converted — a markup sink "
              "that dropped its bytes would report on a document the page's own write was supposed to build");
        n = strlen(text);
        html_parse_document_write(lxb_html_interface_document(dom), (const lxb_char_t *)text, n);
        JS_FreeCString(ctx, text);
    }
    return JS_STEP_DONE;
}

static const IdlStepDecl DW_DECL = {
    dw_step, sizeof(DwState), dw_visit, NULL,
    "HTML §8.4.3 the document write steps", DW_STEPS
};

/* ---- §8.4.2 close() ------------------------------------------------------------------------------------- */

/* HTML §8.4.2 "Closing the input stream". A PLAIN body and not a machine: its six steps run no page code that
   this engine can reach. Steps 1 and 2 are the same two throws §8.4.3 opens with and stay here beside them;
   steps 3-6 are the input stream's own lifetime and live with the steps that created it
   (core/html/document_open.c), which is where the script-created-parser record is kept. */
static JSValue js_doc_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_document_t *dom;

    (void)argc; (void)argv; (void)magic;
    if (!(dom = dw_receiver(ctx, this_val))) return JS_EXCEPTION;
    if (dw_is_xml_document(dom))                                                  /* STEP 1 */
        return JS_ThrowDOMException(ctx, "InvalidStateError", "document.close on an XML document");
    /* STEP 2's throw-on-dynamic-markup-insertion counter is decided for the reason §8.4.3's step 7 is. */
    document_close_input_stream(ctx, this_val, dom);                              /* STEPS 3-6 */
    return JS_UNDEFINED;
}

/* ---- §8.4.1 open() -------------------------------------------------------------------------------------- */

/* HTML §8.4.1 "Opening the input stream" declares TWO overloads, and Web IDL §3.6 "Overload resolution" tells
 * them apart by ARGUMENT COUNT alone — their type lists are two and three long, so an `argcount` of three
 * removes the first entry outright and anything less removes the second:
 *
 *     [CEReactions] Document open(optional DOMString unused1, optional DOMString unused2);
 *     [CEReactions] WindowProxy? open(USVString url, DOMString name, DOMString features);
 *
 * "The unused1 and unused2 arguments are ignored, but kept in the IDL to allow code that calls the function
 * with one or two arguments to continue working" — so the first overload's whole body is the document open
 * steps and its arguments are read by nothing. The second IS §7.2.2's window open steps and belongs to the
 * Window component; it is a different algorithm with a different return type reached through the same name,
 * which is why the arity test is the member's own step and not a guess. */
static JSValue js_doc_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_document_t *dom;

    (void)argv; (void)magic;
    if (argc >= 3) {
        /* §3.6 step 4 has removed the two-argument entry, so this call IS `open(url, name, features)`. */
        DFAIL("§8.4.1's THREE-ARGUMENT `open` was called — Web IDL §3.6 \"Overload resolution\" selects the "
              "second entry by argument count, and that entry's method steps are \"if this is not fully "
              "active, throw an \\\"InvalidAccessError\\\" DOMException; return the result of running the "
              "WINDOW OPEN STEPS with url, name and features\", which is HTML §7.2.2.1 \"Opening and closing "
              "windows\" and belongs to the Window component beside `window.open` (core/frame/navigable.c's "
              "navigable_open is what it reaches). It is not this algorithm with extra arguments: it returns a "
              "WindowProxy, it opens no input stream, and it never touches this document");
        return JS_UNDEFINED;
    }
    if (!(dom = dw_receiver(ctx, this_val))) return JS_EXCEPTION;
    if (dw_is_xml_document(dom))                                                  /* STEP 1 */
        return JS_ThrowDOMException(ctx, "InvalidStateError", "document.open on an XML document");
    /* STEP 2's throw-on-dynamic-markup-insertion counter is decided for the reason §8.4.3's step 7 is. */
    if (!document_open_steps(ctx, this_val, dom)) return JS_EXCEPTION;
    /* "Return document" — the RECEIVER, which is what the page compares against its own `document`. */
    return JS_DupValue(ctx, this_val);
}

/* ---- declaration and install ----------------------------------------------------------------------------- */

void document_write_init(JSContext *ctx)
{
    /* `(TrustedHTML or DOMString)... text` — one declared position, applied to every argument from there on.
       §2's TrustedHTML does not exist, so the union has one live arm and the position is a DOMString; the day
       the type exists this becomes a union row and nothing else about the member moves. */
    static const IdlArgType TEXT_TAIL[] = { IDL_DOMSTRING };

    /* §8.4.1's first overload is `optional DOMString unused1, optional DOMString unused2` and its second is
       three REQUIRED strings; declared as three optional DOMStrings, the arity the page called with reaches
       the body, which is where Web IDL §3.6's count-based choice between the two entries is made. The third
       entry's `USVString url` is not written here because that entry's algorithm is not this one — the body
       hands it back rather than converting for a member it does not implement. */
    static const IdlArgType OPEN_ARGS[3] = { IDL_DOMSTRING, IDL_DOMSTRING, IDL_DOMSTRING };

    DCHECK(g_id_write < 0 && g_id_writeln < 0 && g_id_close < 0 && g_id_open < 0,
           "document_write_init ran twice — the four declarations are the AGENT's and are made once in it");
    g_id_write = idl_method_id_step(ctx, TEXT_TAIL, 1, NULL, 0, &DW_DECL, DW_WRITE);
    idl_variadic();
    idl_optional_from(0);
    g_id_writeln = idl_method_id_step(ctx, TEXT_TAIL, 1, NULL, 0, &DW_DECL, DW_WRITELN);
    idl_variadic();
    idl_optional_from(0);
    g_id_close = idl_method_id(ctx, NULL, 0, js_doc_close, 0);
    g_id_open = idl_method_id(ctx, OPEN_ARGS, 3, js_doc_open, 0);
    idl_optional_from(0);
    /* §8.4.1's record of which parser a `close()` may end — the AGENT's key, minted beside these
       declarations because it is the same one-per-agent statement. */
    document_open_init(ctx);
}

void document_write_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_write >= 0 && g_id_writeln >= 0 && g_id_close >= 0 && g_id_open >= 0,
           "HTML §8.4's members were installed before they were declared — the declaration is the AGENT's and "
           "the install is the REALM's");
    /* Web IDL §3.7.7: an operation whose only argument is variadic has `length` 0. */
    idl_install_method(ctx, proto, "write", g_id_write);
    idl_install_method(ctx, proto, "writeln", g_id_writeln);
    idl_install_method(ctx, proto, "close", g_id_close);
    /* §3.7.7 again: `length` is the number of REQUIRED arguments of the shortest overload, and §8.4.1's first
       entry declares both of its own optional — so `document.open.length` is 0 even though the second entry
       has three required arguments. */
    idl_install_method(ctx, proto, "open", g_id_open);
}

void document_write_free(JSRuntime *rt)
{
    DCHECK(g_id_write >= 0, "HTML §8.4's members were released in an agent that never declared them");
    g_id_write = g_id_writeln = g_id_close = g_id_open = -1;
    /* The key document_write_init minted, given back by its declarer — `rt` and not a context, because the
       Symbol outlives every realm and is freed with the runtime. */
    document_open_free(rt);
}
