/* HTML §8.4 Dynamic markup insertion — see document_write.h for why this is its own component and for the two
   mechanisms `document.write` is waiting on before it can INSERT. What is here is the whole of the algorithm
   that decides WHAT is written, which is the half the @S sink is made of. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
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

/* §8.4.3 step 4 and §8.4.4's own — the standard's SINK NAME, which is what a Trusted Types violation report
   names and what makes two sinks distinguishable in one report. Indexed by the magic above so the pair cannot
   come apart from the `lineFeed` argument they are declared beside. */
static const char *const DW_SINK[2] = { "Document write", "Document writeln" };

/* ---- the receiver -------------------------------------------------------------------------------------- */

/* WEB IDL §3.7.5's BRAND CHECK — a TypeError thrown AT THE CALL and NOT an engine invariant, for the reason
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
    if (!html_parse_insertion_point_defined(dom)) {                               /* STEP 9 */
        /* §8.4.3 step 9.1 is the pair of counters that make an undefined insertion point a silent return, and
           step 9.2 is "run the document open steps with document". NEITHER EXISTS HERE YET, and the reason is
           ONE mechanism rather than three unrelated gaps — see document_write.h: this engine finishes a
           document's parse before running any of its scripts, so §13.2.7 "The end" has already set the
           insertion point to undefined for EVERY document by the time a page can call this, and the fix is a
           live document parser whose tree construction routes through the per-flow DOM delta. Until that
           exists there is nothing for step 9.2 to open onto: a script-created parser feeding the shared tree
           would be writes belonging to no flow. */
        DFAIL("§8.4.3 \"document.write()\" step 9 was reached with a CONCRETE string and §13.2.3.5's insertion "
              "point undefined — this document's parse ran to §13.2.7 \"The end\" before its scripts were "
              "seeded, so there is no input stream to insert into. Build it in this order: (1) route lexbor's "
              "§13.2.6 tree construction through solver/dom_cow.h's chokepoint, because a live parser's "
              "inserts are shared-baseline writes no flow's delta captures; (2) open the ACTIVE document's "
              "parse with html_parse_document_open and close it at §13.2.7's own moment, the lifecycle stage "
              "that moves the readiness to \"interactive\"; (3) build §8.4.1's document open steps here — they "
              "need \"erase all event listeners and handlers\", an exported \"replace all\", and the ENTRY "
              "global object's Document for step 4's same-origin check — and with them §8.4.3 step 9.1's "
              "unload and ignore-destructive-writes counters, the second of which §4.12.1 \"execute the "
              "script element\" raises around an external or module script");
        return JS_STEP_DONE;
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
   this engine can reach. Steps 1 and 2 are the same two throws §8.4.3 opens with; step 3 — "if there is no
   script-created parser associated with this, then return" — is where every call currently ends, because a
   script-created parser is created by §8.4.1's document open steps and by nothing else, and those do not
   exist here. Steps 4-6 (the explicit "EOF" character, the pending-parsing-blocking-script return, and running
   the tokenizer) arrive with them.
   IT IS NOT A NO-OP MEMBER. §8.4.2's throws are observable, and a page that calls `document.close()` at the
   end of a write sequence must find a function there — an absent one takes the whole flow down at a call the
   bundle believed it had covered, which is the same defect as the absent `write` this component is here for. */
static JSValue js_doc_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_document_t *dom;

    (void)argc; (void)argv; (void)magic;
    if (!(dom = dw_receiver(ctx, this_val))) return JS_EXCEPTION;
    if (dw_is_xml_document(dom))                                                  /* STEP 1 */
        return JS_ThrowDOMException(ctx, "InvalidStateError", "document.close on an XML document");
    /* STEP 2's throw-on-dynamic-markup-insertion counter is decided for the reason §8.4.3's step 7 is. */
    /* STEP 3 — and it is asserted rather than assumed, because the assertion is what will fail on the day a
       script-created parser first exists and steps 4-6 have still not been written. §13.2.3.5: a parser
       standing in its PROCESS window has a defined insertion point, and the only parser that reaches this
       member with one open is one §8.4.1 created. */
    DCHECK(!html_parse_insertion_point_defined(dom),
           "§8.4.2 close() step 3 found a document with a LIVE input stream — this engine creates a parser "
           "only through core/html/html_parse.c's document parse, which closes it at §13.2.7 \"The end\", so "
           "an open one here is §8.4.1's script-created parser having come into existence without steps 4-6 "
           "of this algorithm: insert the explicit \"EOF\" character, return if the pending parsing-blocking "
           "script is non-null, and run the tokenizer to it");
    return JS_UNDEFINED;                                                          /* STEP 3 */
}

/* ---- declaration and install ----------------------------------------------------------------------------- */

void document_write_init(JSContext *ctx)
{
    /* `(TrustedHTML or DOMString)... text` — one declared position, applied to every argument from there on.
       §2's TrustedHTML does not exist, so the union has one live arm and the position is a DOMString; the day
       the type exists this becomes a union row and nothing else about the member moves. */
    static const IdlArgType TEXT_TAIL[] = { IDL_DOMSTRING };

    DCHECK(g_id_write < 0 && g_id_writeln < 0 && g_id_close < 0,
           "document_write_init ran twice — the three declarations are the AGENT's and are made once in it");
    g_id_write = idl_method_id_step(ctx, TEXT_TAIL, 1, NULL, 0, &DW_DECL, DW_WRITE);
    idl_variadic();
    idl_optional_from(0);
    g_id_writeln = idl_method_id_step(ctx, TEXT_TAIL, 1, NULL, 0, &DW_DECL, DW_WRITELN);
    idl_variadic();
    idl_optional_from(0);
    g_id_close = idl_method_id(ctx, NULL, 0, js_doc_close, 0);
}

void document_write_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_write >= 0 && g_id_writeln >= 0 && g_id_close >= 0,
           "HTML §8.4's members were installed before they were declared — the declaration is the AGENT's and "
           "the install is the REALM's");
    /* Web IDL §3.7.7: an operation whose only argument is variadic has `length` 0. */
    idl_install_method(ctx, proto, "write", 0, g_id_write);
    idl_install_method(ctx, proto, "writeln", 0, g_id_writeln);
    idl_install_method(ctx, proto, "close", 0, g_id_close);
    /* `open()` IS DELIBERATELY NOT HERE. §8.4.1's two overloads are `Document open(optional DOMString unused1,
       optional DOMString unused2)` and `WindowProxy? open(USVString url, DOMString name, DOMString features)`;
       the first needs the document open steps' three missing primitives (document_write.h names them) and the
       second IS §7.2.2's window open steps, which belong to the Window component. An honestly absent member is
       the forcing function — and engine/idlgen.mjs's audit reports it by name every build, which is the one
       place a reader should learn it rather than from a comment that can go stale. */
}

void document_write_free(void)
{
    DCHECK(g_id_write >= 0, "HTML §8.4's members were released in an agent that never declared them");
    g_id_write = g_id_writeln = g_id_close = -1;
}
