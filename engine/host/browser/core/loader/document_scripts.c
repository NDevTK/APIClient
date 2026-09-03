/* Document script inventory + bundle identity — see document_scripts.h. */
#include "core/loader/document_scripts.h"
#include "check.h"
/* HTML §4.12.1.1 "Processing model" says "Let source text be el's child text content" and links that phrase to
   DOM §4.11 "Interface Text", which ranges over Text NODES — an interface, so a CDATASection is one. Asking
   the tree layer for a nodeType-keyed walk instead answered "" for every `<script><![CDATA[ … ]]></script>` an
   XHTML document ships, and "" is exactly the value the next step already has a meaning for. */
#include "core/dom/text_content.h"
#include "core/dom/selector_match.h"
#include <string.h>
#include <stdlib.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

static lxb_status_t scr_collect_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) {
    struct scr_ctx *c = vp; (void)s;
    if (c->n >= c->cap) { int nc = c->cap ? c->cap * 2 : 8;
        lxb_dom_element_t **n = realloc(c->els, (size_t)nc * sizeof(lxb_dom_element_t *)); if (!n) return LXB_STATUS_OK; c->els = n; c->cap = nc; }
    c->els[c->n++] = lxb_dom_interface_element(node);
    return LXB_STATUS_OK;
}

void dom_collect_scripts(lxb_html_document_t *dom, struct scr_ctx *out) {
    out->els = NULL; out->n = 0; out->cap = 0;
    if (!dom) return;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)"script", 6);
    if (!list) { lxb_css_parser_destroy(p, true); return; }
    lxb_selectors_t *sel = lxb_selectors_create();
    if (!sel || lxb_selectors_init(sel) != LXB_STATUS_OK) { if (sel) lxb_selectors_destroy(sel, true); lxb_css_selector_list_destroy_memory(list); lxb_css_parser_destroy(p, true); return; }
    /* THE SAME HOST ANSWERS AS THE AGENT'S ARENA. This walk's selector is the literal "script", so no
       host-language pseudo-class can reach it today — and that is a reason to install the table, not to skip
       it: the day this string grows a `:defined`, the alternative is a NULL call inside lexbor rather than a
       different answer, and core/dom/selector_match.h's contract is that one selector has one meaning. */
    lxb_selectors_host_cb_set(sel, selector_match_host_cb(), NULL);
    lxb_selectors_find(sel, lxb_dom_interface_node(dom), list, scr_collect_cb, out);
    /* the selector list owns its OWN css memory arena (lxb_css_selectors_parse allocates it), separate from the
       parser's — destroying the parser does NOT free it, so free it explicitly or it leaks per call. */
    lxb_selectors_destroy(sel, true); lxb_css_selector_list_destroy_memory(list); lxb_css_parser_destroy(p, true);
}

/* Infra's ASCII whitespace: TAB, LF, FF, CR, SPACE — the set §4.12.1 strips from the type attribute. */
static int scr_ascii_ws(lxb_char_t c) {
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

/* HTML §4.12.1 "prepare the script element", the type-string steps. THE ANSWER IS THE RETURN VALUE: this was
   `script_is_exec(el, &is_mod)`, which computed the module bit into an out-parameter that BOTH of its callers
   declared and never read — so a module script became a classic one at the compile and its top-level `await`
   was reported as a SyntaxError. `script_is_importmap` was a second, narrower parser of the same attribute and
   had no caller at all; both are gone, replaced by the one question the spec asks once. */
ScriptType script_block_type(lxb_dom_element_t *el) {
    size_t tyl = 0;
    const lxb_char_t *ty = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tyl);
    char tb[128];
    size_t b = 0, e = tyl, k;

    if (!ty) return SCRIPT_TYPE_CLASSIC;   /* no type attribute -> "text/javascript" -> classic */
    /* STRIP LEADING AND TRAILING ASCII WHITESPACE, which the spec does before any of the four matches: the
       attribute value is authored text, so `<script type=" module ">` is a module script. Matching the raw
       bytes made it a data block that never ran and never said so. */
    while (b < e && scr_ascii_ws(ty[b])) b++;
    while (e > b && scr_ascii_ws(ty[e - 1])) e--;
    if (e == b) return SCRIPT_TYPE_CLASSIC;   /* empty type attribute -> "text/javascript" -> classic */
    /* A type string longer than this buffer is none of the four matches below: each is an exact ASCII
       case-insensitive compare against a keyword of at most 16 bytes, and a JavaScript MIME type essence is
       shorter still. Truncating it and comparing would answer about a string the element does not have. */
    if (e - b >= sizeof tb) return SCRIPT_TYPE_NONE;
    for (k = b; k < e; k++) { char ch = (char)ty[k]; tb[k - b] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }
    tb[e - b] = 0;
    if (strstr(tb, "javascript") || strstr(tb, "ecmascript")) return SCRIPT_TYPE_CLASSIC;
    if (strcmp(tb, "module") == 0)           return SCRIPT_TYPE_MODULE;
    if (strcmp(tb, "importmap") == 0)        return SCRIPT_TYPE_IMPORTMAP;
    if (strcmp(tb, "speculationrules") == 0) return SCRIPT_TYPE_SPECULATIONRULES;
    return SCRIPT_TYPE_NONE;   /* HTML's null: no script is executed */
}

/* IS THE ATTRIBUTE PRESENT — asked of the attribute LIST and never through `get_attribute`, which answers NULL
   for an attribute whose value is absent. `<script defer>` is exactly that: lexbor's tree construction only
   calls `attr_set_value_wo_copy` when the token had a `value_begin`, so a boolean attribute in its usual
   valueless spelling has `attr->value == NULL` and `get_attribute` cannot distinguish it from an absent one —
   which is the whole of what `async` and `defer` are. */
static bool scr_has_attr(lxb_dom_element_t *el, const char *name, size_t len) {
    return lxb_dom_element_has_attribute(el, (const lxb_char_t *)name, len);
}

/* §4.12.1's LAST STEPS — see ScriptSchedule. The branches are the standard's, in the standard's order; the one
   thing worth naming is WHICH question the first `if` is: the tail splits on whether the element's result is
   still "uninitialized", which is the same set as "an external classic script, or any module script" — the two
   the algorithm hands to a fetch. Everything else has already been marked as ready, so it reaches
   "immediately execute the script element". */
ScriptSchedule script_block_schedule(lxb_dom_element_t *el, ScriptType ty, bool parser_inserted,
                                     bool force_async) {
    bool external = scr_has_attr(el, "src", 3);   /* the ATTRIBUTE, not a non-empty value */

    DCHECK(script_type_executes(ty),
           "a script element's schedule was asked for a type that executes nothing — an import map and a set of "
           "speculation rules are registered rather than run, and §4.12.1's null type runs nothing at all, so "
           "none of them joins any of the Document's script queues");
    DCHECK(!(parser_inserted && force_async),
           "a parser-inserted script element was said to have `force async` true — §4.12.1 has the HTML and XML "
           "parsers set it FALSE on every element they insert, so the two cannot both hold and the caller has "
           "read one of them off something that is not the element");
    if (!external && ty != SCRIPT_TYPE_MODULE) return SCRIPT_SCHED_IMMEDIATE;
    if (scr_has_attr(el, "async", 5) || force_async) return SCRIPT_SCHED_ASAP;
    if (!parser_inserted)                           return SCRIPT_SCHED_IN_ORDER_ASAP;
    /* "if el has a defer attribute or el's type is module" — defer has no effect on a module script because a
       module is in this list either way. */
    if (scr_has_attr(el, "defer", 5) || ty == SCRIPT_TYPE_MODULE) return SCRIPT_SCHED_WHEN_PARSED;
    return SCRIPT_SCHED_PARSER_BLOCKING;
}

unsigned document_bundle_id(lxb_html_document_t *dom) {
    struct scr_ctx c; dom_collect_scripts(dom, &c);
    uint32_t bh = 2166136261u;
    for (int i = 0; i < c.n; i++) {
        lxb_dom_element_t *el = c.els[i];
        size_t sl = 0;
        /* THE SAME PRESENCE TEST document_exec_scripts MAKES, because identity is over the scripts that RUN: an
           element with a `src` attribute never runs its child text, so hashing that text would give a document
           the identity of a program it does not have. */
        bool has_src = scr_has_attr(el, "src", 3);
        const lxb_char_t *src = has_src ? lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl)
                                        : NULL;
        if (has_src) {
            if (src && sl) {
                for (size_t k = 0; k < sl; k++) { bh ^= src[k]; bh *= 16777619u; }   /* the src URL IS the id */
                bh ^= '|'; bh *= 16777619u;
            }
            continue;
        }
        if (!script_type_executes(script_block_type(el))) continue;   /* data block: not part of JS identity */
        /* THE SAME CONCATENATION THE PROGRAM ITSELF IS TAKEN FROM — core/dom/text_content.h — because identity
           is over the bytes that RUN. Two spellings of "this element's text" would let a document's id and its
           program disagree, which for an XHTML bundle is precisely what happened: the id was computed over an
           empty string for every script whose body is a CDATA section.
           THE BYTES ARE HASHED UNSIGNED. `char` is signed on the hosts this builds for, so a byte at or above
           0x80 would sign-extend into the mix and the id would differ from the same document's under an
           unsigned buffer — a difference in the frontier key, from a cast. */
        size_t tl = 0; char *txt = dom_child_text_content(lxb_dom_interface_node(el), &tl);
        if (tl) { for (size_t k = 0; k < tl; k++) { bh ^= (unsigned char)txt[k]; bh *= 16777619u; } bh ^= '|'; bh *= 16777619u; }
        free(txt);
    }
    free(c.els);
    return bh ? bh : 1;
}

DocScripts document_exec_scripts(lxb_html_document_t *dom) {
    struct scr_ctx c; dom_collect_scripts(dom, &c);
    DocScripts ds = { NULL, NULL, NULL, NULL, NULL, 0 };
    if (c.n) {
        ds.bodies = calloc((size_t)c.n, sizeof(char *));
        ds.srcs   = calloc((size_t)c.n, sizeof(char *));
        ds.types  = calloc((size_t)c.n, sizeof(ScriptType));
        ds.sched  = calloc((size_t)c.n, sizeof(ScriptSchedule));
        /* §4.12.1.1's "execute the script element" is a switch on EL, so the row keeps it — see the header. */
        ds.els    = calloc((size_t)c.n, sizeof(lxb_dom_element_t *));
        if (!ds.bodies || !ds.srcs || !ds.types || !ds.sched || !ds.els) {
            free(ds.bodies); free(ds.srcs); free(ds.types); free(ds.sched); free(ds.els); free(c.els);
            ds.bodies = ds.srcs = NULL; ds.types = NULL; ds.sched = NULL; ds.els = NULL;
            return ds;
        }
    }
    /* Each executable script becomes its OWN entry — never concatenated, so its top-level let/const stays
       script-scoped. An EXTERNAL one takes its position with only a URL; the scheduler parks the flow there and
       the host's reply fills the slot. A data block (json/importmap) is parsed, never run.
       THE OUTER LOOP IS §13.2.7's, AND THE INNER ONE IS THE TREE'S — one pass per rank over the elements in
       document order, which is a stable sort by rank without a sort: within one of the standard's milestones the
       document's own order is kept, and a `defer`red or `async` script written between two inline ones lands
       after both. It is three walks of a `<script>` list and not one because the alternative is a permutation
       array every reader would then have to apply. */
    for (int rank = 0; rank < SCRIPT_RUN_RANK_N; rank++)
    for (int i = 0; i < c.n; i++) {
        lxb_dom_element_t *el = c.els[i];
        size_t sl = 0;
        bool has_src = scr_has_attr(el, "src", 3);
        const lxb_char_t *src = has_src ? lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl)
                                        : NULL;
        ScriptType ty = script_block_type(el);
        ScriptSchedule sc;
        if (!script_type_executes(ty)) continue;   /* a data block is parsed, never run */
        /* A PARSE PRODUCT IS PARSER-INSERTED, WITH `force async` FALSE — §4.12.1 states both of the parser, so
           they are facts about this scan and not defaults it picks. An element a SCRIPT inserted goes through
           html_script.c's half of §4.12.1 instead, where neither is true. */
        sc = script_block_schedule(el, ty, /*parser_inserted*/true, /*force_async*/false);
        if (script_sched_run_rank(sc) != rank) continue;
        /* …WHICH IS WHY RANK 2 HOLDS ONLY THE SET HERE. The `list of scripts that will execute in order as soon
           as possible` is reached by an element that is NOT parser-inserted, and every element of this scan is,
           so a row of that list means the schedule was read off something a parse did not build. */
        DCHECK(sc != SCRIPT_SCHED_IN_ORDER_ASAP,
               "a parsed document's script inventory holds a member of the `list of scripts that will execute "
               "in order as soon as possible` — §4.12.1 reaches that list only for an element with a null "
               "parser document, and this scan states the parser inserted every element it walks");
        if (has_src) {
            /* §4.12.1's src BRANCH is entered on the ATTRIBUTE, and its second step is `src` being the empty
               string: "queue an element task … to fire an event named error at el, and return". So the element
               runs NOTHING — not its child text, which is what a value-length test let through
               (`<script src="">alert(1)</script>` ran the alert). The error event is
               core/html/html_script.h's html_script_queue_error and is NOT owed HERE: this is a pure DOM SCAN
               of a parsed tree with no realm and no flow under it, so queuing a task from it would be the
               baseline walk ISSUING work rather than RECORDING it. The caller that holds a realm performs
               §4.12.1.1's steps over these rows and is the party that fires. */
            if (src && sl) {
                /* THE ROW IS NOT CONDITIONAL ON THE ALLOCATION, for the same reason the inline branch below is
                   not: a `<script src>` this scan drops is an external program the document never fetches, and
                   it leaves the table looking exactly like a document that shipped one fewer script. An `if
                   (u)` here was that hole with a NULL check's face on it. */
                char *u = malloc(sl + 1);

                CHECK(u != NULL,
                      "OOM copying a `<script src>` URL out of the tree — a row this scan does not write is a "
                      "program the document never fetches, and nothing downstream can tell that from a "
                      "document that shipped one fewer script");
                memcpy(u, src, sl); u[sl] = 0;
                ds.srcs[ds.n] = u; ds.bodies[ds.n] = NULL; ds.types[ds.n] = ty; ds.sched[ds.n] = sc;
                ds.els[ds.n] = el;
                ds.n++;
            }
            continue;
        }
        /* §4.12.1.1's STEPS 5 AND 6, AND THE EMPTY ANSWER IS STEP 6 BEING TAKEN. "Let source text be el's
           child text content." then "If el has no `src` attribute, and source text is the empty string, then
           return." So a zero length here is the standard's own return for an element with no program — a
           `<script>` holding only a comment — and it is stated as that rather than tested as a hole.
           IT USED TO BE A HOLE, AND THAT IS THE WHOLE OF WHY THIS FILE CHANGED. The concatenation came from a
           nodeType-keyed walk that cannot see a CDATASection, so every `<script><![CDATA[ … ]]></script>` in an
           XHTML document measured zero, took this return, and left NO ROW — a document whose own code silently
           did not run, indistinguishable from one that shipped none. The copy-if-malloc-succeeded below it was
           the second half of the same shape: a failed allocation ALSO left no row, so a page's program could
           vanish with nothing said. The buffer is now the concatenation's own — allocated once, NUL-terminated
           and ADOPTED into the table — so there is no second allocation to fail and no branch to drop a row. */
        {
            size_t tl = 0;
            char *txt = dom_child_text_content(lxb_dom_interface_node(el), &tl);

            if (tl == 0) { free(txt); continue; }
            ds.types[ds.n] = ty; ds.sched[ds.n] = sc;
            ds.els[ds.n] = el;
            ds.bodies[ds.n++] = txt;
        }
    }
    /* EVERY ROW OF THIS TABLE IS ONE ELEMENT OF THIS TREE, asserted at the producer because this is the one
       place it is structurally true: both branches above write `els` beside the body or the address they wrote.
       Downstream the column may legitimately be absent — a host driving a synthesized program list has rows no
       `<script>` produced — so a consumer cannot check it, and this is where a forgotten write would be. */
    for (int k = 0; k < ds.n; k++)
        DCHECK(ds.els[k] != NULL,
               "a document's script inventory holds a row with no `script` element — HTML §4.12.1.1 "
               "\"execute the script element\" is a switch on EL and its classic arm sets §3.1.7's "
               "currentScript to it, so a row without one runs its program with that member left null");
    free(c.els);
    return ds;
}

void doc_scripts_free(DocScripts *ds) {
    if (!ds || !ds->bodies) return;
    for (int i = 0; i < ds->n; i++) { free(ds->bodies[i]); if (ds->srcs) free(ds->srcs[i]); }
    /* `els` holds BORROWED pointers into the tree the scan walked — the array is this table's, the nodes are
       the document's, so the array is freed and nothing in it is. */
    free(ds->bodies); free(ds->srcs); free(ds->types); free(ds->sched); free(ds->els);
    ds->bodies = ds->srcs = NULL; ds->types = NULL; ds->sched = NULL; ds->els = NULL; ds->n = 0;
}
