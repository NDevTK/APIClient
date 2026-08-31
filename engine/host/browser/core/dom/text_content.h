/* DOM §4.11 Interface Text's TWO CONCATENATIONS, over DOM's OWN TEST FOR WHAT COUNTS — and nothing else.
 *
 * WHAT IT IS. §4.11 defines both in one place and in one shape: "The child text content of a node node is the
 * concatenation of the data of all the Text node children of node, in tree order." and "The descendant text
 * content of a node node is the concatenation of the data of all the Text node descendants of node, in tree
 * order." They differ in the LINKS they follow and in nothing else, which is why they are one file.
 *
 * WHY IT IS A COMPONENT AND NOT A CALL TO THE TREE LAYER. The phrase both definitions turn on is "Text node",
 * and DOM answers that BY INTERFACE: §4.12 Interface CDATASection is `interface CDATASection : Text { };`, so
 * a CDATASection IS a Text node and its data is part of both concatenations. §4.4 Interface Node says the same
 * thing about the member these concatenations exist for, in a clause that is the whole of this file's reason
 * to exist — "To get text content with a node node, return the following, switching on the interface node
 * implements". A tree layer switches on nodeType instead, and nodeType names CDATASection separately from
 * Text, so a walk written that way computes a DIFFERENT FUNCTION: it agrees with §4.11 on every document that
 * contains no CDATA section and disagrees silently on every one that does.
 *   THE DISAGREEMENT IS SILENT IN THE WORST DIRECTION — it returns the EMPTY STRING, which is a value every
 * consumer already has a meaning for. HTML §4.12.1.1 Processing model reads "Let source text be el's child
 * text content" and then "If el has no src attribute, and source text is the empty string, then return", so a
 * `<script>` in an XHTML document whose program is written as XML §2.7 CDATA Sections' [18] CDSect — which is
 * the ONLY way to write a program containing `<` in XHTML, and therefore how such documents are ordinarily
 * written — reached that step and RETURNED. The page's own code did not run, nothing crashed, and the document
 * was indistinguishable from one whose `<script>` was empty. That is CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS
 * performed by a walk rather than by a `||`: the producer cannot see the node, and the consumer's existing
 * meaning for "nothing" conceals it.
 *
 * SO THE INTERFACE TEST IS ANSWERED IN ONE PLACE. `dom_node_is_text` is that place; every walk here is written
 * over it, and so is core/dom/node.c's resumable §4.4 walk, which cannot share this file's loop because it
 * rests between nodes. The rule CLAUDE.md §A-QUESTION-SOME-ENTRIES-ASK states is what forced the shape: a
 * question some entries ask and others do not is one missing capability wearing two names, and this one wore
 * five — a script's source text, a document's bundle identity, `textContent`, a `<style>` sheet's rules and a
 * `<textarea>`'s default value each asked it separately and four of them got a different answer than DOM's.
 *
 * OWNERSHIP: the result is a `malloc`'d, NUL-terminated buffer the CALLER frees with `free`, and it is NEVER
 * NULL — an empty concatenation is a real answer (a `<script>` holding only a comment has one) and returning
 * NULL for it would hand every caller back the same two-meanings-one-value problem this file exists to end.
 * `*len` is the byte count, which is what a caller must use: §4.12.1's inline source text is the one string in
 * this engine that can legitimately hold a U+0000, so a `strlen` over it is a truncation. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_TEXT_CONTENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_TEXT_CONTENT_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* DOM §4.12 Interface CDATASection: `interface CDATASection : Text { };`. THE ONE PLACE this engine answers
   "is this node a Text node", which is the question §4.4 Interface Node's `get text content` switches on and
   the one §4.11 Interface Text's two concatenations range over. A caller that spells the nodeType comparison
   itself is asking a different question and will disagree with the standard on an XML document. */
bool dom_node_is_text(const lxb_dom_node_t *node);

/* DOM §4.11 Interface Text: "The child text content of a node node is the concatenation of the data of all the
   Text node children of node, in tree order." This is what HTML §4.12.1.1 Processing model's step "Let source
   text be el's child text content" names, and what §4.12.1 The script element's `text` getter returns. */
char *dom_child_text_content(const lxb_dom_node_t *node, size_t *len);

/* DOM §4.11 Interface Text: "The descendant text content of a node node is the concatenation of the data of
   all the Text node descendants of node, in tree order." This is the Element and DocumentFragment arm of §4.4
   Interface Node's `get text content`. "Descendant" is DOM §1.1's relation over the NODE TREE, so the walk
   follows child links and nothing else — a subtree reached only through a host pointer (§4.7 Interface
   DocumentFragment's host) is a different tree and its text is not this node's. */
char *dom_descendant_text_content(const lxb_dom_node_t *node, size_t *len);

#endif
