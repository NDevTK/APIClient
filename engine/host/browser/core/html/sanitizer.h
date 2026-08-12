/* HTML §8.6 — THE SANITIZER API. See sanitizer.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_SANITIZER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_SANITIZER_H
#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* Declared once per AGENT (from element_init, beside §13.3's serializer), installed per REALM. */
void sanitizer_init(JSContext *ctx);
void sanitizer_free(void);

/* §8.6.4's "GET A SANITIZER INSTANCE FROM OPTIONS", answered as the CONFIGURATION rather than as the Sanitizer
   object: the caller of it is `set and filter HTML`, whose only use of the instance is its configuration, and
   §8.6.4's `sanitize` reads exactly that. `options` is the dictionary the declaration built, `safe` is the
   member's own flag, and the result is an OWNED canonical SanitizerConfig — already through §8.6.2's
   canonicalize, so nothing downstream has to ask whether it was.
   It runs none of the page's code for the arms it answers: a Sanitizer object's configuration is this engine's
   own, and "default" and the absent-member defaults are built here. A page-supplied SanitizerConfig DICTIONARY
   is the arm that would, and it crashes naming the machine to build rather than being converted by a C walk
   that cannot park inside the page's iterator. */
JSValue sanitizer_config_from_options(JSContext *ctx, JSValueConst options, bool safe);

/* §8.6.2's BRAND — "is this value a Sanitizer". The `sanitizer` option is a union whose arms are told apart by
   it, and it is this component's question because the class is. */
bool sanitizer_is(JSValueConst v);

/* §8.6.4's "SANITIZE" AND "INNER SANITIZE STEPS", as a walk that rests at every node — the fragment is the
   page's markup, so the walk is of the page's size and a flow inside it must be able to park and resume.
 *
 * IT IS DRIVEN BY THE MEMBER THAT PARSED THE FRAGMENT rather than being a machine of its own, because §8.6.4
 * states it as step 7 of `set and filter HTML`: the fragment between the parse and the replacement is that
 * member's, and a second machine would need the fragment handed across a boundary that does not exist. So the
 * stages below are expanded into the HOSTING member's stage list (element.c's), which is what makes each of
 * them a rest point the driver can assert on and a parked flow can name.
 *
 * THE STAGE CONSTANTS ARE ONE X-LIST expanded in two files: here-and-in-this-file's own enum, numbered from
 * zero, and in the hosting member's enum, numbered after its own stages. `stage_base` is the difference, taken
 * from the host at `begin` — the host passes SAN_CHILD, which is the first name the list produces, so the two
 * numberings are stated by the same list rather than by a constant either file could get wrong. */
#define SANITIZE_STAGES(X) \
    X(SAN_CHILD,   "HTML §8.6.4 inner sanitize steps 1.1-1.4.3.1 (one child of node's children: the assert " \
                   "that it is one of the five kinds, the DocumentType/Text continue, and a comment's and a " \
                   "processing instruction's own list tests)") \
    X(SAN_ELEMENT, "HTML §8.6.4 inner sanitize steps 1.5.1-1.5.5 (this element's name and namespace against " \
                   "the configuration's element lists, and the descent into a <template>'s template " \
                   "contents)") \
    X(SAN_SHADOW,  "HTML §8.6.4 inner sanitize step 1.5.6 (the descent into a shadow host's shadow root)") \
    X(SAN_ATTRS,   "HTML §8.6.4 inner sanitize steps 1.5.7-1.5.9.5.3 (one attribute of this element's " \
                   "attribute list against the global and per-element lists, and the javascript: URL and " \
                   "animating-URL removals)") \
    X(SAN_DESCEND, "HTML §8.6.4 inner sanitize step 1.5.10 (the recursive invocation over this element's own " \
                   "children)") \
    X(SAN_NEXT,    "HTML §8.6.4 inner sanitize step 1 (the step to the next child in tree order)") \
    X(SAN_REMOVE,  "HTML §8.6.4 inner sanitize steps 1.3.1 / 1.4.2.1 / 1.4.3.1 / 1.5.3.1 / 1.5.4.1 (remove " \
                   "child — one node of its subtree per step, deepest first)") \
    X(SAN_POP,     "HTML §8.6.4 inner sanitize step 1 (this level's children are exhausted, so the recursive " \
                   "invocation it belongs to returns)")

/* A LEVEL of the walk: the element a descent was made from, the PRIVATE TREE that was in effect when it was
   made, and what the algorithm does when that descent returns. The three `after` values are the three descents
   §8.6.4 makes — a template's contents (step 1.5.5), a shadow root (step 1.5.6) and the element's own children
   (step 1.5.10) — and each resumes at a different step.
   THE ROOT IS PART OF THE LEVEL because two of those three descents leave the tree: a `<template>`'s template
   contents and a shadow root are each a DETACHED node of their own, so a node inside one belongs to THAT
   private tree and not to the fragment — and dom_cow's private operations are declared over the root the node
   is actually in. Restoring it with the level is what makes a removal inside a template's contents legal and a
   removal after that descent still name the fragment. */
enum { SAN_AFTER_TEMPLATE = 0, SAN_AFTER_SHADOW, SAN_AFTER_CHILDREN };
typedef struct { lxb_dom_node_t *node; lxb_dom_node_t *root; uint8_t after; } SanLevel;

typedef struct {
    JSValue config;             /* the canonical SanitizerConfig (OWNED) */
    uint8_t handle_js_urls;     /* §8.6.4's `handleJavascriptNavigationUrls`, which is `safe` */
    int     stage_base;         /* the hosting member's constant for this list's first stage */
    /* THE PRIVATE TREE THIS WALK MAY WRITE. Every removal here is a write to a tree the parse itself built and
       nothing else has ever seen, so it goes through dom_cow's PRIVATE operations rather than the capturing
       chokepoint — a capture of it would put the whole parsed fragment's structure into the delta, and its
       unapply would re-insert nodes into a fragment that no longer exists. */
    lxb_dom_node_t *root;
    lxb_dom_node_t *tree_root;  /* the private tree `cur` is in — the fragment, or a template contents/shadow */
    lxb_dom_node_t *dead_root;  /* the same for the subtree being removed, which descends into templates too */
    lxb_dom_node_t *cur;        /* §8.6.4 step 1's `child` */
    lxb_dom_node_t *next_sib;   /* the sibling a removal resumes at, taken BEFORE the node is detached */
    lxb_dom_node_t *dead;       /* the node of the doomed subtree this step frees; deepest first */
    lxb_dom_node_t *doomed;     /* the child being removed — the subtree's root */
    /* HOW MANY LEVELS THE REMOVAL ITSELF PUSHED. The removal borrows the same stack the walk uses, which is
       sound because a removal finishes before the walk resumes — but "the private tree I am in is empty" means
       two different things depending on whether THIS removal descended into it or the WALK did, and only the
       count tells them apart. Without it a removal inside a template's contents pops the walk's own level. */
    int       dead_depth;
    lxb_dom_attr_t *attr;       /* step 1.5.9's cursor */
    SanLevel *stack;
    int       sp, scap;
} SanitizerWalk;

/* `config` is CONSUMED. `root` is the node whose CHILDREN are sanitized (§8.6.4 sanitize's `node`), and it is
   also the private tree the walk is allowed to write. */
void sanitizer_walk_begin(JSContext *ctx, SanitizerWalk *w, lxb_dom_node_t *root, JSValue config,
                          bool safe, int stage_base);
/* ONE step. Returns JS_STEP_YIELD while there is more to do and 0 when the walk is over; the caller owns what
   happens next, which for §8.6.4 is step 8's replacement. */
int  sanitizer_walk_step(JSContext *ctx, JSStepHdr *hdr, SanitizerWalk *w);
void sanitizer_walk_visit(JSContext *ctx, SanitizerWalk *w, JSStepVisit *v);
void sanitizer_walk_release(JSContext *ctx, SanitizerWalk *w);

#endif
