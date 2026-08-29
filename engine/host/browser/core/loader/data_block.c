/* HTML §4.12.1's data blocks — see data_block.h for what this is and why the value is a triple. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/loader/data_block.h"
#include "core/loader/document_scripts.h"
#include "core/html/html_script.h"
#include "solver/concolic.h"

int data_block_is(lxb_dom_element_t *el)
{
    /* THE TAG QUESTION IS ASKED WHERE THE ELEMENT LIVES. §4.12.1 is about `script` elements and its
       type-string steps are about nothing else, so script_block_type answers CLASSIC for any element with no
       `type` attribute — an ordinary `<div>` included. Without the brand test in front of it every element
       whose `type` attribute happens to be unmatched (`<input type=whatever>`, `<button type=x>`) would be a
       data block, and its text would leave the DOM as unknown input. */
    if (!el || !html_script_is(lxb_dom_interface_node(el)))
        return 0;
    return script_block_type(el) == SCRIPT_TYPE_NONE;
}

/* IS THIS ID SPELLABLE AS A HOLE? A shape names its unknown between braces and endpoint.c's consumer reads
   one back with `/\{([^}\/]+)\}/`, so a name carrying a brace, a slash or whitespace is a hole the report can
   print and the reviewer can never substitute — and a name that ends the segment early is worse than a
   generic one, because it takes the parameter's value with it. The set kept here is what an author writes an
   id out of; anything else falls back to the position, which is a fact about the document just as much. */
static int id_is_spellable(const lxb_char_t *s, size_t n)
{
    size_t i;

    if (n == 0)
        return 0;
    for (i = 0; i < n; i++) {
        lxb_char_t c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            continue;
        if (c == '_' || c == '-' || c == '.' || c == '$')
            continue;
        return 0;
    }
    return 1;
}

/* Pre-order over the document, the walk that gives a `script` element its document-order position. */
static lxb_dom_node_t *next_in(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    if (n->first_child)
        return n->first_child;
    while (n != root) {
        if (n->next)
            return n->next;
        n = n->parent;
        if (!n)
            return NULL;
    }
    return NULL;
}

/* WHICH BLOCK THESE BYTES CAME OUT OF, AS THE DOCUMENT ITSELF NAMES IT — the provenance half of §@H's shape,
   which says WHO must supply the parameter. Two spellings and both are the document's own:
     `script#__NEXT_DATA__`  — its `id`, which is also how the author's own code finds it (getElementById),
     `script[3]`             — its position among the document's `script` elements in tree order, which is
                               `document.scripts[3]` and is what a reader opens the document and counts.
   A NAME THAT IS NOT UNIQUE IS ONE NAME FOR SEVERAL UNKNOWNS, and every predicate over any of them would then
   decide all of them — the same trap absent.c states for a truncated path. Ids are unique in a conforming
   document and a position is unique in any document, so each spelling identifies one block.
   THE POSITION COSTS A DOCUMENT WALK and is paid at the READ rather than cached, because a cache would be a
   second answer to "where is this element" that the tree can move out from under: `document.scripts` is a
   live list and a block inserted before this one renumbers it. A data block's text is read once by the code
   that parses it, so the walk is bounded by that and not by a loop. */
static char *data_block_name(lxb_dom_element_t *el)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el), *root, *n;
    size_t idl = 0;
    const lxb_char_t *id = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"id", 2, &idl);
    char *out;
    int i = 0;

    if (id && id_is_spellable(id, idl)) {
        size_t len = strlen("script#") + idl + 1;
        out = (char *)malloc(len);
        CHECK(out != NULL, "data_block: OOM naming a data block by its id");
        snprintf(out, len, "script#%.*s", (int)idl, (const char *)id);
        return out;
    }
    /* AN ELEMENT WITH NO OWNER DOCUMENT HAS NO POSITION IN ONE, and it is not a state to default past: every
       element this engine hands to script was created by a document (§4.5's createElement) or parsed into
       one, so a NULL here is the node heap and the DOM disagreeing about what an element is. */
    DCHECK(node->owner_document != NULL,
           "a data block was named while belonging to no document — an element's position among the "
           "document's script elements is the only name it has when it carries no id, and an element with no "
           "owner document was never created by one");
    root = node->owner_document ? lxb_dom_interface_node(node->owner_document) : node;
    for (n = root; n != NULL; n = next_in(n, root)) {
        if (n == node)
            break;
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && html_script_is(n))
            i++;
    }
    out = (char *)malloc(sizeof "script[]" + 20);
    CHECK(out != NULL, "data_block: OOM naming a data block by its position");
    snprintf(out, sizeof "script[]" + 20, "script[%d]", i);
    return out;
}

JSValue data_block_wrap_text(JSContext *ctx, lxb_dom_element_t *el, JSValue text)
{
    char *src, *shape;
    size_t n;
    JSValue r;

    /* A HOST THAT IS NOT EXPLORING GETS THE BROWSER'S OWN ANSWER, which is the string §4.12.1's own reader
       would see. This is the same seam concolic_source_wrap draws for an attacker source and it is drawn for
       the same reason: the value SEMANTICS are installed unconditionally, and only whether one is MINTED
       turns on the host. Asked before anything is composed so a conformance run pays one compare. */
    if (!concolic_is_exploring() || !data_block_is(el))
        return text;
    /* THE BYTES ARE THE EXAMPLE AND THERE IS NEVER A SECOND ONE. A door that handed this an already-minted
       value would be a second entry wrapping the first's answer, and the composed shape would then name a
       parse of a parse — which is a provenance no read performed. */
    DCHECK(!concolic_is(text),
           "a data block's text reached the mint already carrying a provenance — two doors have wrapped one "
           "read, and the value would report a derivation the run never made");
    DCHECK(JS_IsString(text),
           "a data block's child text content is not a string — §4.12.1.1 reads a data block as the element's "
           "child text content and every door that computes it concatenates Text data, so a non-string here "
           "is a door handing over something that is not this element's text");

    src = data_block_name(el);
    n = strlen(src) + 3;
    shape = (char *)malloc(n);
    CHECK(shape != NULL, "data_block: OOM spelling the provenance of a data block's content");
    snprintf(shape, n, "{%s}", src);
    r = concolic_new(ctx, shape, src, text);   /* consumes `text` as the example */
    free(shape);
    free(src);
    return r;
}
