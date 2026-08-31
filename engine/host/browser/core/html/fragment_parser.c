/* HTML §13.4 "Parsing HTML fragments" — THE FRAGMENT PARSE, as ONE operation, because there are six members
   that do it and they differ only in where the result goes. `context` is the element whose parsing state the
   fragment is parsed IN — a `<tr>` is dropped anywhere but inside a table, and that is the tree builder's
   rule, not something a caller chooses. The parsed nodes are handed to the placement, which inserts each
   through the per-flow chokepoints.
   LEXBOR MUST NOT RUN PAGE CODE. That is what lets a parser — a state machine with a great deal of internal
   position — live inside an engine whose flows suspend and resume at any depth: the parse holds no continuation
   across anything the page can preempt, so it never has to be suspended and never has to be part of a snapshot.
   It completes inside one opcode over bytes, and the tree builder never runs a `<script>` it parses: what
   §13.2.4.5's scripting mode decides is what the FRAG_FEED boundary STAMPS on the ones it produced. Under
   INERT — the default, and what four of the six members take — that stamp is `already started`, which
   §4.12.1 step 1 reads when the placement's insertion steps prepare the element, so the script is dead. Under
   FRAGMENT the stamp is not applied and the script runs when the fragment reaches a document, which is the
   whole of what §13.2.4.5 means by "executed as soon as they are inserted".
   Re-entry is what a violation would look like: page code running mid-parse and reaching one of these again.
   Asserted rather than assumed, because the day it stops holding is the day a half-built tree ends up inside
   another flow's delta.

   WHERE THIS FILE CAME FROM AND WHY: see core/html/fragment_parser.h. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/html/fragment_parser.h"
#include "core/dom/attr_list.h"
#include "core/dom/document.h"       /* §8.5.4's fragment parsing algorithm steps step 2 asks the DOCUMENT */
#include "core/html/declarative_shadow.h"
#include "core/html/html_parse.h"    /* the ONE place an HTML parser is made — that header owns the token bytes */
#include "core/html/html_script.h"
#include "core/html/media_element.h"
#include "core/html/event_handler_attribute.h"
#include "core/html/sanitizer.h"
#include "core/idl_args.h"
#include "solver/dom_cow.h"

/* THE FRAGMENT PARSE, AS A MACHINE — the last drive-to-completion left beside the insertion it feeds.
 * `lxb_html_document_parse_fragment` tokenises and tree-builds the whole markup inside one opcode, so
 * `container.innerHTML = bigMarkup` held the scheduler for the length of the markup. The insertion steps next
 * to it were converted first and this was still the larger half.
 *
 * LEXBOR HAS THE SEAM ALREADY: chunk_begin / chunk_process / chunk_end is exactly a resumable parse, and the
 * `lexbor_in` machinery behind it exists so a token can span two chunks. So the parser is fed ONE BYTE per
 * step. A byte is the finest unit lexbor offers — it will not expose a token boundary — and it needs no chosen
 * quantum, which is the thing a "parse 4096 bytes then yield" would have to invent and defend.
 *
 * A PRIVATE PARSER PER PARSE, and that is not an optimisation — it is what makes yielding legal at all.
 * `lxb_html_document_parse_fragment` uses the DOCUMENT's parser, and the moment a parse can suspend, a second
 * flow can start its own; two interleaved parses sharing one tokenizer and one open-element stack would
 * corrupt both. chunk_begin takes the parser explicitly and builds its own temporary document, so a parser per
 * parse is independent by construction. The old `in_parse` re-entry DCHECK is gone with it: it asserted that a
 * parse never overlaps, which is now exactly what this machine is built to allow.
 *
 * AND IT IS THE ONE MACHINE LEFT THAT CANNOT BE FORKED, which is a fact about the ENGINE and not about the
 * page. The reason written here used to be that the tree builder runs no script, so no branch can fork the
 * flow while this machine is on its chain — an argument about what the page happens to be doing, and the
 * scheduler's own reasons to snapshot a parked flow (a higher-value sibling, RAM pressure, a cold-tier
 * eviction, a cross-session resume) never ask that. The real reason is one sentence: between two FRAG_FEED
 * steps this machine holds an lxb_html_parser_t, and lexbor exposes no way to copy one. What must be built is
 * named at fragment_parse_unforkable, which is where the fork aborts. */

void fragment_parse_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FragmentParse *s = st;
    v->val(ctx, &s->compliant);
    v->buf(ctx, (void **)&s->html, s->len ? s->len + 1 : 0);
    v->val(ctx, &s->san_config);
    sanitizer_walk_visit(ctx, &s->san, v);
}

/* THE TWO CAPABILITIES LEFT, AND THEIR NAMES. Between two FRAG_FEED steps the markup is `html`, the position
   in it is `off`, the placement is `where`/`anchor` and fragment_parse_visit declares the references — all of
   that parks already. What does not is `parser`, and the sentence the fork prints is the specification of what to build,
   because the reader of a @WHY is standing at the clone with nothing else to go on.

   AND THE SECOND ONE WAS SAYING NOTHING AT ALL, which is worse than the first. This predicate tested `parser`
   and only `parser`, so the instant the feed ended and the parser was destroyed the machine reported itself
   FORKABLE — while still OWNING the whole tree the parse had produced (`frag`, deep-destroyed by
   fragment_parse_release, with `node` the cursor into it and §8.6.4 set and filter HTML's sanitizer walk
   standing inside it) and, for §8.5.5 step 5 and §8.5.7 step 6, an `own_context` element that is in no tree
   and that fragment_parse_release also destroys. Neither is in fragment_parse_visit, because JSStepVisit has
   no operation for a private DOM tree — so a fork taken at a FRAG_PLACE or FRAG_CLEAR or SAN_* rest point
   handed two arms ONE tree: both would place the same nodes into their own documents and
   both fragment_parse_releases would destroy it. Those rest points are reachable, because the placement's
   §4.2.3 insertion steps run page code (a custom element's connectedCallback), which is where a concolic branch
   forks. Nothing said so, and a fork that silently shares an owned tree is precisely the corruption a fork
   abort exists to prevent — so this now answers for the whole machine and not for one of its fields.
   IT IS ASKED AT THE FORK, and it used to be a DCHECK inside fragment_parse_visit instead, whose comment sent
   the next reader to "give the parser a real ownership declaration" if it ever fired. That instruction was wrong twice
   over: `visit` now has three consumers, so the assert fires on a TEARDOWN where nothing is being cloned, and
   no ownership declaration would have helped there because a tokenizer has nothing to declare. A machine must
   never learn which consumer is visiting it; this is how it answers the fork alone.

   THE FIRST OF THE TWO HALVES IS BUILT, AND THE MESSAGE NO LONGER ASKS FOR IT. That is not bookkeeping: a
   @WHY's failure mode is that it stays accurate about the SPEC and goes wrong about THIS tree, so a sentence
   still naming the tree-builder copy would read as authoritative while sending the next reader to write
   core/html/tree_construction.c a second time. This message has already been wrong that way once — it said to
   copy "the partially built fragment and its TEMPORARY DOCUMENT", and there is no such unit, because
   lxb_dom_document_init with a non-NULL owner inherits the real document's arenas and hashes and stamps every
   parsed node with the REAL document. What the fork needs is the SUBTREE plus a node->node map to move the tree
   builder's arrays onto it, which is what that component now is.
   WHAT IS LEFT IS ORDERED, and the two clauses below are in that order: the private-tree declaration first,
   because it is the one a fork reaches TODAY and the one whose clone half is already written, and §13.2.5's
   tokenizer second. See the header above fragment_parse_step for the rest of the ordering. */
const char *fragment_parse_unforkable(const void *st)
{
    const FragmentParse *s = st;
    const char *why = xml_fragment_unforkable(&s->xf);

    /* §14.4's HALF ANSWERS FOR ITSELF, because what it owns between two steps is its own — an XML parse handle
       and a private tree — and a driver that listed those fields would be a second statement of another
       component's ownership. Asked first because it is the half a flow standing in FRAG_XML is inside. */
    if (why != NULL)
        return why;
    if (s->frag != NULL || s->own_context != NULL)
        return "a fragment parse cannot be forked between its parse and its placement — this machine OWNS the "
               "tree the parse produced (`frag`, which fragment_parse_release deep-destroys, with `node` the "
               "cursor into it and §8.6.4 set and filter HTML's sanitizer walk standing inside it) and, for "
               "§8.5.5 step 5 and §8.5.7 step 6, an `own_context` element that is in no tree and that "
               "fragment_parse_release destroys too. fragment_parse_visit declares neither, because JSStepVisit "
               "has no operation for a PRIVATE DOM TREE, so the sibling arm would share one tree with the "
               "original: two arms placing the same nodes and two releases destroying them. "
               "BUILD THAT OPERATION — a `v->tree` whose three consumers do different work the way v->reexec's "
               "do: the CLONE deep-copies the subtree into the same document arena and re-points every cursor "
               "through a node->node map, which is copy_subtree in core/html/tree_construction.c (written for "
               "the tree-builder copy and needing only to be exported with its map); the TEARDOWN is "
               "dom_cow_destroy_private; the FINGERPRINT folds the node addresses. Then fragment_parse_visit declares "
               "`frag` with `node`, `own_context`, and the sanitizer walk declares its own cursors, and this "
               "clause deletes";
    return s->parser
         ? "a fragment parse cannot be forked mid-parse — the TREE-BUILDER half is built "
           "(core/html/tree_construction.c: html_tree_construction_copy deep-copies the partial subtree into "
           "the same document arena and moves mode, original_mode, open_elements, active_formatting, "
           "template_insertion_modes, pending_table, parse_errors, form, fragment, foster_parenting, "
           "frameset_ok and scripting onto the copy through a node->node map), and it takes the COPY'S "
           "TOKENIZER as an argument because lxb_html_tree_init binds the two. BUILD THAT: lxb_html_tokenizer's "
           "state and state_return, the start/pos/end temp buffer, the begin/last/markup/temp cursors — which "
           "point into THIS machine's `html`, and fragment_parse_visit hands the sibling a COPY of that buffer, so they "
           "are rebased by offset and not copied, which is also the HtmlInputRebase the tree half already asks "
           "for — the token under construction with its dobj_token/dobj_token_attr pools and mraw temp "
           "strings, the entity SBST cursor, and the `tree` back-pointer its own header calls a leak "
           "abstraction (foreign-content and RCDATA/RAWTEXT tokenization read the tree through it, so it is "
           "repointed at the tree copy and the two halves cannot be cloned separately). Then the fork assembles "
           "an lxb_html_parser_t from the pair, taking `root` and `form` from the copy. AND THAT STILL DOES "
           "NOT PARK. A snapshot crosses a session, so every field must have a NAME, and `state`/`state_return` "
           "have none: tokenizer/state*.c define 182 state functions and 172 of them are static, behind the 12 "
           "entry points declared in tokenizer/state*.h — a raw code pointer means nothing to the build that "
           "resumes the snapshot, and the file that would have to export them cannot be edited (lexbor is a "
           "pinned pristine clone engine/build.mjs re-clones). Cloning buys the FORK and not the cold tier; "
           "the tier needs HTML §13.2.5's tokenizer as an engine component whose state is a spec-named enum, "
           "feeding this tree builder through lxb_html_tree_construction_dispatcher"
         : NULL;
}

/* WHAT NO DECLARATION NAMES: a lexbor element, a lexbor parser, and THE PARSE'S OWN TREE.
   `compliant`, the sanitizer's `config`, the markup copy and the sanitizer walk's level stack are all named by
   fragment_parse_visit — the two buffers as `buf`, which is why they are allocated with the RUNTIME's allocator: the
   fork's copy of a `buf` is js_malloc'd, so a sibling arm releasing a plain malloc'd one would hand the wrong
   allocator a runtime block, and the accounting the runtime keeps of both would be wrong in each direction. */
void fragment_parse_release(JSContext *ctx, void *st)
{
    FragmentParse *s = st;
    (void)ctx;
    /* §14.4's machine owns an XML parse handle and, until the take, the private tree it built — released
       through its own entry for the reason the fork asks it its own question: what it holds is its to name.
       Idempotent and inert for an HTML parse, whose `xf` is all-zero. */
    xml_fragment_release(&s->xf);
    /* §8.5.5 step 5's / §8.5.7 step 6's `body` never entered a tree, so nothing else will ever free it. */
    if (s->own_context) {
        dom_cow_destroy_private(lxb_dom_interface_node(s->own_context), /*with_children*/ true);
        s->own_context = NULL;
    }
    /* THE THROW PATH OWNS THE PARSER TOO. A flow dropped mid-parse would otherwise leak a tokenizer, an
       open-element stack and the temporary document behind them. */
    if (s->parser) {
        /* AND IT OWNS WHAT THE PARSER BUILT, which is the half that was missing. chunk_end HANDS BACK the root
           element §13.4 step 8 created — the whole partially built fragment hangs under it — and the value was
           discarded here, so a flow abandoned between the first byte and the placement leaked every node the
           parse had produced. Nothing else can ever collect them: they are allocated out of the REAL document's
           arena (see core/html/tree_construction.c for why §13.4's temporary document is not an arena of its
           own — lxb_dom_document_init INHERITS the real document's) and they are in
           no tree, so no delta, no discard and no document destroy names them.
           A machine cannot COPY what it does not OWN, so this is also the fork's first subproblem, discharged:
           the sibling arm's copy of the partial tree is exactly the object this statement frees. */
        lxb_dom_node_t *root = lxb_html_parse_fragment_chunk_end(s->parser);
        /* AND THE DECLARATION THAT NAMED THIS PARSE, released on the abandoned path exactly as on the
           completed one: the tree it is keyed on is freed by the destroy below, and a record left behind would
           be found by the next parser allocated at that address and called ITS document. */
        dom_cow_parse_release(s->parser->tree);
        lxb_html_parser_destroy(s->parser);
        s->parser = NULL;
        /* NULL is chunk_end's failure answer, and it destroyed the root itself on the way out. Otherwise it is
           the same object `frag` names once the feed has finished, so the one statement below covers both
           shapes — the abandoned parse and the abandoned placement. */
        if (root) s->frag = root;
    }
    /* WHATEVER OF THE PARSE IS LEFT. After a completed placement this is NULL, because FRAG_PLACE's terminal
       destroys the emptied root and gives up its claim there; a placement or a §8.6.4 set and filter HTML filter that threw
       half-way leaves the un-placed remainder here, still holding nodes. */
    if (s->frag) {
        dom_cow_destroy_private(s->frag, /*with_children*/ true);
        s->frag = NULL;
    }
}

/* Set the machine up for a parse of `html` into `where` around `anchor`, parsed in `context`'s tree-building
   context, under §13.2.4.5's `scripting` mode. `html` is COPIED because the JSString it came from is released
   before the first suspension. */
void fragment_parse_begin(JSContext *ctx, FragmentParse *s, lxb_dom_element_t *context, lxb_dom_node_t *anchor,
                       int where, const char *html, bool clear_first, bool allow_declarative,
                       FragScriptingMode scripting)
{
    /* The clear is `replace all within TARGET`, and the target is the anchor — which for a <template> is its
       content fragment rather than the element. Only the children-replacing member asks for it, which is what
       lets the placement's reference child be computed before the clear rather than after it. */
    DCHECK(!clear_first || where == FRAG_INTO_CHILDREN,
           "a fragment parse asked to replace its target's children at a position that is not FRAG_INTO_CHILDREN — "
           "the placement's reference child is fixed before the clear, which only holds for an append");
    DCHECK(scripting == FRAG_SCRIPTING_INERT || scripting == FRAG_SCRIPTING_FRAGMENT,
           "HTML §8.5.4 \"The innerHTML property\"'s fragment parsing algorithm steps step 1 is \"Assert: "
           "scriptingMode is either Inert or Fragment\", and §13.4 \"Parsing HTML fragments\" step 1 asserts it "
           "again — Normal and Disabled belong to a DOCUMENT parse and there is no member that may ask for one "
           "here");
    /* HTML §8.5.4 "The innerHTML property"'s FRAGMENT PARSING ALGORITHM STEPS STEP 2 — "If target's node
       document is an XML document, then return the result of invoking the XML fragment parsing algorithm given
       target and markup." IT IS ASKED HERE, at the ONE point all six members converge on, and not at any of
       them: a dispatch deciding WHICH PARSER a set of bytes gets that some entries ask and others do not is
       one missing capability wearing two names, and the entry that skips it does not report the absence — it
       reports an unrelated subsystem failing on input that subsystem should never have been shown.
       THAT IS EXACTLY WHERE IT LANDED BEFORE THIS LINE EXISTED. Nothing asked, so an XML document's
       `el.innerHTML = markup` ran the HTML tree builder over XML and the first thing to notice was
       core/dom/attr_list.c's parse-boundary attribute correction, whose own DCHECK says the tree it was handed
       belongs to an XML document — a true sentence about the tree, pointing at the attribute component, for a
       parse that was never HTML. The crash was real and the file was wrong.
       THE ANSWER RIDES THE STATE rather than branching here, because the step that acts on it is a stage away:
       the two algorithms differ in the PARSER and its boundary seams and agree exactly on where the parsed
       nodes go, so FRAG_XML replaces FRAG_FEED and the placement below is one placement for both. §14.4 itself
       is core/html/xml_fragment.h.
       §14.4 STEP 1 IS ALREADY DISCHARGED HERE — "Let context be target if target is an Element; otherwise
       target's host" — because that is the same resolution §13.4's parse context comes from and the member has
       already made it. What arrives is the Element, which is what both algorithms take. */
    s->is_xml = document_is_xml_of(lxb_dom_interface_node(context)->owner_document) ? 1 : 0;
    s->context = context;
    s->anchor = anchor;
    s->where = (uint8_t)where;
    s->clear_first = clear_first;
    s->allow_declarative = allow_declarative;
    s->scripting = (uint8_t)scripting;
    s->len = strlen(html);
    s->html = js_malloc(ctx, s->len + 1);
    CHECK(s->html != NULL, "the fragment parse could not copy its markup");
    memcpy(s->html, html, s->len + 1);
    s->off = 0;
    s->node = NULL;
}

/* §8.5.4 step 5 / §8.5.5 step 7 / §8.5.6 step 6 / §8.5.7 step 7 / §8.6.4 set and filter HTML step 9 — WHERE
   THE PARSE GOES NEXT, which is one answer reached from two places: the end of the feed, and the end of
   §8.6.4's filter between them. Written once because the two must not drift — a filter that resumed at the
   wrong one of these would place an unsanitized fragment or clear the target twice. */
void fragment_parse_placement(FragmentParse *s, JSStepHdr *hdr)
{
    /* THE PARSE ALWAYS PRODUCED ONE. chunk_begin creates the root element before the first byte and only an
       allocation failure clears it, which is now fatal at the feed — so the `if (!s->frag) go to DONE` this
       used to open with described a state that cannot arise, and the same test stood in two more places. */
    DCHECK(s->frag != NULL, "the fragment placement was reached with no parsed fragment");
    if (s->clear_first) {
        s->node = s->anchor->first_child;
        hdr->stage = FRAG_CLEAR;
        return;
    }
    s->node = s->frag->first_child;
    hdr->stage = FRAG_PLACE;
}

/* ONE STEP of the parse-and-place. Returns JS_STEP_YIELD while there is more, or 0 when the fragment is in the
   tree. Every caller is a member body that returns whatever this returns. */
int fragment_parse_step(JSContext *ctx, JSStepHdr *hdr, FragmentParse *s)
{
    switch (hdr->stage) {
    case FRAG_CLEAR: {
        /* `Replace all with fragment within target` REMOVES the target's children first, and a page's existing
           subtree is as big as the page. The parse is already finished by the time this runs, which is the
           order the setter states. */
        lxb_dom_node_t *next;
        if (!s->node) {
            DCHECK(s->frag != NULL, "the fragment clear finished with no parsed fragment to place");
            s->node = s->frag->first_child;
            hdr->stage = FRAG_PLACE;
            return JS_STEP_YIELD;
        }
        next = s->node->next;
        dom_cow_remove_child(s->node);
        s->node = next;
        return JS_STEP_YIELD;
    }
    case FRAG_FEED:
        /* HTML §8.5.4's fragment parsing algorithm steps STEP 2, acted on. The answer was taken at
           fragment_parse_begin — the one point every member converges on — and this is the one place it is
           acted on, so no member and no other stage asks which parser its bytes get. §14.4 steps 3-4 open the
           parse in the same call that routes to it, because neither can suspend. */
        if (s->is_xml) {
            xml_fragment_begin(ctx, &s->xf, s->context, s->html, s->len);
            hdr->stage = FRAG_XML;
            return JS_STEP_YIELD;
        }
        if (!s->parser) {
            lxb_dom_node_t *cn = lxb_dom_interface_node(s->context);
            /* THROUGH core/html/html_parse.h, WHICH IS WHERE AN HTML PARSER IS MADE. A fragment parse reads
               attribute values like any other parse, and the ones tree construction does not adopt — a
               duplicate attribute, an attribute of a token an insertion mode ignores, a DOCTYPE's ids
               (§13.2.6.4.7 ignores the token outright) — come out of the AGENT's text arena exactly as a
               document parse's do: `lxb_html_parse_fragment_chunk_begin` points the tokenizer at `doc->text`
               of the temporary document, whose arenas are this document's. A parser built here with
               `lxb_html_parser_create` directly would leak one allocation per such value in every
               `innerHTML =`. The other half of that ownership is on the REAL document, installed at
               dom_document_create, which is where §13.4's inherited temporary document gets it from. */
            s->parser = html_parse_new_parser();
            CHECK(s->parser != NULL, "the fragment parser could not be created");
            /* EVERY ONE OF THESE STATUSES WAS DROPPED, and dropping them is not a missing report — it is a
               WRONG DOM. Each of the three entries responds to a failure by destroying the root, clearing
               parser->root and moving the parser to ERROR, from which every later chunk_process returns
               WRONG_STAGE and discards its byte in silence; the feed then ran to the end of the markup and
               chunk_end handed back NULL, so `el.innerHTML = markup` left the element EMPTY and said nothing.
               Every path that sets a non-OK status here is an allocation failure or an internal table lookup
               that cannot miss — lexbor's tree builder reaches lxb_html_tree_process_abort only from those,
               never from malformed markup, which it handles by the spec's own error rules — so this is
               CHECK's case (allocation, dev AND release) and not a DCHECK's. */
            CHECK(lxb_html_parse_fragment_chunk_begin(s->parser,
                      lxb_html_interface_document(cn->owner_document), cn->local_name, cn->ns)
                  == LXB_STATUS_OK,
                  "the fragment parse could not be started");
            /* WHOSE TREE §13.2.6 IS ABOUT TO BUILD — §13.4 "Parsing HTML fragments" step 3's "Let document be
               a Document node whose type is html", which lexbor creates inside the call above and hangs the
               root element under. It is FLOW-PRIVATE in the strongest sense available: this statement made it,
               nothing between the two runs the page's code, and the placement below is what moves its contents
               into a tree another flow can reach — through the capturing chokepoint, one node at a time. The
               declaration is keyed on this parse's own tree builder, so it survives the per-byte suspension
               below rather than standing open while a sibling flow runs. */
            dom_cow_parse_declare(s->parser->tree,
                                  lxb_dom_interface_node(s->parser->tree->document),
                                  DOM_PARSE_ROOT_PRIVATE);
            return JS_STEP_YIELD;
        }
        if (s->off < s->len) {
            /* ONE BYTE. lexbor's incoming-buffer machinery is what makes a token able to span two of these. */
            CHECK(lxb_html_parse_fragment_chunk_process(s->parser,
                      (const lxb_char_t *)s->html + s->off, 1) == LXB_STATUS_OK,
                  "the fragment parse failed on a byte of its markup");
            s->off++;
            return JS_STEP_YIELD;
        }
        s->frag = lxb_html_parse_fragment_chunk_end(s->parser);
        /* AFTER the end and BEFORE the parser goes: `lxb_html_tree_end` runs the EOF token through the
           insertion modes, which is §13.2.6 writing this tree one last time, and `lxb_html_parser_destroy`
           frees the tree the declaration is keyed on. The release reads nothing but that key — the temporary
           document it declared as its root has already been destroyed by the call above. */
        dom_cow_parse_release(s->parser->tree);
        lxb_html_parser_destroy(s->parser);
        s->parser = NULL;
        CHECK(s->frag != NULL, "the fragment parse produced no root element");
        /* THE PARSE'S NODES BELONG TO THE REAL DOCUMENT, and everything after this line depends on it: the
           placement moves them into that document's tree with no §4.5 adopt, and their tag ids and attribute
           names are only meaningful against that document's hashes. It holds because §13.4's temporary
           document is created against this one and lxb_dom_document_init INHERITS its arenas — see
           core/html/tree_construction.c, where the same fact is what makes a fork's copy of this parse need no
           §4.5 adopt and re-intern no name, and where it is asserted per copied node. */
        DCHECK(s->frag->owner_document == lxb_dom_interface_node(s->context)->owner_document,
               "a fragment parse produced nodes belonging to a document other than its context element's — the "
               "placement below inserts them with no adopt, so their node document would be wrong for every "
               "flow that reads it");
        /* The same parse boundary the document has: tree construction produces attributes in the NULL
           namespace, and lexbor stamps every one it creates with the ELEMENT's instead. Corrected here,
           before a single node of this fragment is moved into a tree anything can read — and ONCE. It was
           written twice, on the same node under two comments saying the same thing in different words, so
           every `el.innerHTML = markup` walked the whole parsed fragment's attributes twice. */
        dom_attr_normalize_parsed(s->frag);
        /* HTML §13.2.6.4.4 'The "in head" insertion mode'`s `script` START TAG, whose three stamps this
           boundary applies to the tree the parse produced. TWO OF THEM ARE UNCONDITIONAL and one is not, and
           the section states each in its own sentence:
             "Set the element's force async to false."  — every parse, which is why html_script_parsed applies
           it whatever the mode is.
             "If the scripting mode is not Fragment, then set the element's parser document to the Document."
           — this engine keeps no parser document on the element (core/html/html_script.h says why: the CALLER
           states it, because the caller is either §13.2.6 tree construction or page code and only one of those
           can be the parser). Under FRAGMENT the placement below reaches §4.2.3's insertion steps, which pass
           `parser_inserted` FALSE — which is precisely the section's own note: "The Fragment scripting mode
           treats parser-inserted scripts as if they were not parser-inserted, allowing, for example, executing
           scripts when applying a fragment created by createContextualFragment()."
             "If the parser's scripting mode is Inert, then set the script element's already started to true.
           (fragment case)" — THE ONE THIS BRANCHES ON. That flag is the only thing that stops a placed script
           from running: the placement goes through dom_cow_append_child, which runs §4.2.3's insertion steps,
           which prepare an inserted `<script>`, and §4.12.1 step 1 is where the flag is read. So under INERT
           the markup's scripts are dead and under FRAGMENT they are live, and NOTHING ELSE about the parse
           differs — which is what makes `range.createContextualFragment("<script>…")` a fragment whose script
           has not run (§4.12.1 step 7: "If el is not connected, then return") and which runs the moment the
           page appends it to a document.
           IT RUNS AT THIS BOUNDARY, beside the namespace correction above and before the declarative-shadow
           conversion below, because the scripts the parse produced are still children of `frag`: the
           conversion moves a `<template>`'s contents into a shadow root, and a walk after it would not reach a
           `<script>` that went with them.
           BEFORE ANY NODE IS PLACED, which is what makes the substitution of one walk for a per-start-tag
           stamp unobservable: this parse runs no page code, so nothing can look at a `script` element between
           the start tag that created it and this statement. */
        html_script_parsed(ctx, s->frag, /*inert*/ s->scripting == FRAG_SCRIPTING_INERT);
        /* HTML §13.2.6.4.4's template start tag, over the fragment this parse just built — the same seam the
           document's parse runs it at, and the reason §13.4 takes `allowDeclarativeShadowRoots` at all.
           THE TOPMOST ELEMENT IS NONE. The step's third condition names "the topmost element in the stack of
           open elements", which for a fragment parse is the root element §13.4 step 8 creates and which lexbor
           does not hand back — so a `<template shadowrootmode>` written at the TOP of the markup is one whose
           parent is the FRAGMENT rather than an element, which declarative_shadow_parsed refuses for exactly
           the reason the third condition exists. That is why `el.setHTMLUnsafe("<template shadowrootmode=open>")`
           leaves a template and does not give `el` a shadow root.
           It runs BEFORE the placement, because a browser runs it during tree construction: the hosts it
           attaches to are still this parse's private nodes, so nothing else can see a half-converted tree. */
        declarative_shadow_parsed(ctx, s->frag, NULL, s->allow_declarative != 0);
        /* HTML §4.8.11.2's "if a media element is created with a src attribute, the user agent must
           immediately invoke the media element's resource selection algorithm", over the same fragment at the
           same boundary and unobservable for the same reason the two statements above are: this parse runs no
           page code, so nothing can look at a `<video src>` between the start tag that created it and here.
           AFTER the declarative-shadow conversion, because a media element inside a `<template shadowrootmode>`
           went with the contents that moved and the walk is shadow-including so that it still finds it.
           §13.4's INERT scripting mode is not a reason to skip this: it marks `script` elements and says
           nothing about media, and a media element is created with its attributes whether or not a script
           would have run. */
        media_element_parsed(ctx, s->frag);
        /* HTML §8.1.8.1 Event handlers OVER THE FRAGMENT, and unlike §4.8.4.3.2 below this one belongs HERE
           rather than in the insertion drain. §13.2.6.1 "Create an element for a token" appends the token's
           attributes to the element it just created, so the attribute change steps run at CREATION — before
           the element is inserted anywhere, and before anything can be dispatched at it. That order is the
           whole of why this matters for `el.innerHTML = '<img src=x onerror=…>'`: the insertion drain is where
           §4.8.4.3.2's image update runs and therefore where the `error` event comes from, so a handler
           installed by that same drain would be racing the dispatch it exists to catch. Installing at creation
           also matches what a browser does for a fragment nobody inserts — `createContextualFragment`'s
           `<svg onload>` has its handler, it simply never fires.
           AFTER the declarative-shadow conversion, and the walk is shadow-including, for the media walk's
           reason: a handler attribute inside a `<template shadowrootmode>` moved with the contents. */
        event_handler_attribute_parsed(ctx, s->frag);
        /* NO §4.8.4.3.2 WALK OVER THIS FRAGMENT, and that is a statement rather than an omission. An `img`'s
           mutation list already contains "the img HTML element insertion steps", and every node of this
           fragment that reaches a document goes through §4.2.3 insert step 7's shadow-including inclusive
           descendant walk — which is the drain a few hundred lines up, where html_image_inserted sits. So
           `el.innerHTML = '<img src=x onerror=…>'` updates its image data exactly once, at the moment the
           element enters the tree. A walk here would ALSO update every fragment that never reaches one — a
           `<template>`'s content, a `createContextualFragment` nobody inserts — whose images a browser does
           not load, and would then update the inserted ones a second time. The media walk above is not the
           same case: §4.8.11.2 starts an element created WITH a `src` whether or not it is connected. */
        /* The reference child is fixed BEFORE anything moves: inserting changes `anchor->next`. The clear that
           may follow cannot move it either — it only ever empties an append target, which
           fragment_parse_begin asserts. */
        s->ref = (s->where == FRAG_INTO_AFTER) ? s->anchor->next
               : (s->where == FRAG_INTO_FIRST_CHILD) ? s->anchor->first_child
               : s->anchor;
        /* §8.6.4 set and filter HTML STEP 8, between the parse and the replacement: the fragment is filtered
           while it is still this parse's private tree, which is the only moment at which removing from it is invisible to
           everything else — and the only moment at which a `<script>` the configuration removes has not yet
           been prepared by the insertion steps. */
        if (s->sanitize) {
            sanitizer_walk_begin(ctx, &s->san, s->frag, s->san_config, s->safe != 0, SAN_CHILD);
            s->san_config = JS_UNDEFINED;   /* the walk owns it now */
            hdr->stage = SAN_CHILD;
            return JS_STEP_YIELD;
        }
        fragment_parse_placement(s, hdr);
        return JS_STEP_YIELD;

    case FRAG_XML: {
        /* HTML §14.4 "Parsing XML fragments" steps 5-11, one item per step — the algorithm is
           core/html/xml_fragment.h's and this is only where its result meets the placement. */
        int r = xml_fragment_step(ctx, &s->xf);

        if (r != 0)
            return r;   /* YIELD, or ABRUPT having thrown §14.4 step 7's or step 8's "SyntaxError" */
        s->frag = xml_fragment_take(&s->xf);
        /* THE SAME INVARIANT THE HTML ARM ASSERTS AT ITS OWN BOUNDARY, and for the same reason: the placement
           moves these nodes into the document with no DOM §4.5 adopt, so their node document must ALREADY be
           the context's. §14.4's reading would put them in a Document of their own and adopt them at step 11's
           append; core/xml/xml_tree.h's two-argument entry is what lets them be created in the right document
           instead, and this is where that claim is checked rather than trusted. */
        DCHECK(s->frag->owner_document == lxb_dom_interface_node(s->context)->owner_document,
               "HTML §14.4's parse produced nodes belonging to a document other than its context element's — "
               "the placement below inserts them with no adopt, so their node document would be wrong for "
               "every flow that reads it");
        /* NONE OF THE HTML PARSE-BOUNDARY SEAMS RUN FOR THIS TREE, which is core/html/domparser.c's XML arm's
           list and its reasons: dom_attr_normalize_parsed would rewrite namespaces Namespaces in XML §6.2
           "Namespace Defaulting"'s expansion just decided, and §13.2.6.4.4's declarative-shadow conversion and §4.8.11.2's media walk
           are triggers stated over an HTML parser's stack of open elements. The one seam that DOES run is
           §14.2's already-started stamp, and it runs inside §14.4's own machine because that is the standard
           that owns it.
           The reference child is fixed BEFORE anything moves, exactly as on the HTML arm. */
        s->ref = (s->where == FRAG_INTO_AFTER) ? s->anchor->next
               : (s->where == FRAG_INTO_FIRST_CHILD) ? s->anchor->first_child
               : s->anchor;
        /* §8.6.4 set and filter HTML STEP 8 reaches the same walk from here, because the filter is a fact
           about the MEMBER and not about which parser produced the tree. */
        if (s->sanitize) {
            sanitizer_walk_begin(ctx, &s->san, s->frag, s->san_config, s->safe != 0, SAN_CHILD);
            s->san_config = JS_UNDEFINED;   /* the walk owns it now */
            hdr->stage = SAN_CHILD;
            return JS_STEP_YIELD;
        }
        fragment_parse_placement(s, hdr);
        return JS_STEP_YIELD;
    }

    case FRAG_PLACE: {
        /* Everything here moves nodes OUT of what the parse just built, which nothing else has ever seen — see
           dom_cow.h. `frag` is the declaration, passed to each operation. */
        lxb_dom_node_t *node = s->node, *next;
        if (!node) {
            dom_cow_destroy_private(s->frag, /*with_children*/ false);
            /* THE CLAIM IS SPENT HERE, the way a delta entry's is — fragment_parse_release owns whatever `frag` still
               names, so leaving the pointer behind after this free is the double free that ownership buys. */
            s->frag = NULL;
            if (s->where == FRAG_INTO_REPLACE) dom_cow_remove_child(s->anchor);
            hdr->stage = FRAG_DONE;
            return 0;
        }
        next = node->next;
        dom_cow_take_private(s->frag, node);   /* out of the private tree; the INSERT below is the shared write */
        switch (s->where) {
        case FRAG_INTO_CHILDREN:    dom_cow_append_child(s->anchor, node); break;
        case FRAG_INTO_BEFORE:
        case FRAG_INTO_REPLACE:     dom_cow_insert_before(s->anchor, node); break;
        case FRAG_INTO_AFTER:       if (s->ref) dom_cow_insert_before(s->ref, node);
                                else dom_cow_append_child(s->anchor->parent, node);
                                break;
        case FRAG_INTO_FIRST_CHILD: if (s->ref) dom_cow_insert_before(s->ref, node);
                                else dom_cow_append_child(s->anchor, node);
                                break;
        default: DFAIL("a fragment was placed with an unknown position"); break;
        }
        s->node = next;
        return JS_STEP_YIELD;
    }
    default:
        DCHECK(hdr->stage == FRAG_DONE, "the fragment machine resumed into a stage it does not have");
        return 0;
    }
}
