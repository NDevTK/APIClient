/* HTML §13.4 "Parsing HTML fragments" — THE FRAGMENT PARSE, as one step machine. See fragment_parser.c.
 *
 * WHY IT IS A COMPONENT AND NOT A BLOCK INSIDE core/dom/element.c, WHERE IT WAS: the same sentence
 * core/dom/element.c already carries about §13.3's serialiser, one algorithm later. That file held the
 * SERIALISING walk until a second interface needed it, and the answer was a component (core/html/
 * fragment_serializer.c) rather than a second copy. §13.4's PARSE reached the same point the moment DOM §5.5's
 * `createContextualFragment` needed it: a Range is not an Element, so the machine could no longer be reached
 * through an Element member's magic, and the two ways out were to export a lexbor parser handle and a
 * SanitizerWalk through core/dom/element.h — a header most of core/dom includes — or to put §13.4 where §13.4
 * belongs. One algorithm, one component; the six members that parse markup are declarations over it and hold
 * no parse of their own.
 *
 * WHAT A MEMBER OWES IT: a state block whose FIRST FIELD is a FragmentParse (an IdlStepDecl's size is a
 * compile-time constant, which is the same contract core/dom/range.h's two exported machines state), the
 * stages below in its `steps` list, and `fragment_parse_visit` / `_release` / `_unforkable` on its
 * declaration. A member that names one of those and not the others has half a declaration. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_FRAGMENT_PARSER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_FRAGMENT_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/html/html.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/html/sanitizer.h"

/* WHERE THE PARSED NODES GO. Not a step of §13.4 — §13.4 returns a DocumentFragment and says nothing about
   what its caller does with it — but the one thing the six members differ in, so it rides the parse rather
   than being a sixth copy of the placement loop. `FRAG_INTO_CHILDREN` with an anchor that IS a
   DocumentFragment is how a member that RETURNS the fragment (§8.5.7) reaches the same loop as one that
   replaces an element's children (§8.5.4). */
enum { FRAG_INTO_CHILDREN = 0, FRAG_INTO_BEFORE, FRAG_INTO_AFTER, FRAG_INTO_FIRST_CHILD, FRAG_INTO_REPLACE };

/* HTML §13.2.4.5 "Other parsing state flags"' PARSER SCRIPTING MODE — the two values §13.4 admits, which its
   own first step asserts ("Assert: scriptingMode is either Inert or Fragment"). Normal and Disabled are the
   other two and belong to a DOCUMENT parse, which is why they are not here: a member reaching this machine is
   invoking the fragment parsing algorithm steps, whose signature is
   `(target, markup, scriptingMode = Inert)` — HTML §8.5.4 "The innerHTML property".
     INERT — §13.2.4.5 verbatim: "Scripts are enabled, however they are marked as already started, essentially
   preventing them from executing. This is the default mode of the HTML fragment parsing algorithm." Four of
   the six members pass nothing and get it.
     FRAGMENT — §13.2.4.5 verbatim: "Scripts are executed as soon as they are inserted into the document as
   part of a the HTML fragment parsing algorithm, ignoring async and defer attributes. This mode is used by
   createContextualFragment()." Two members reach it: HTML §8.5.7 "The createContextualFragment() method" step
   7 passes it outright, and HTML §8.6.4 "Sanitization algorithms"' set and filter HTML step 6 passes it when
   `options["runScripts"]` is true (step 5 is what sets scriptingMode to Inert; step 6 is what overrides it).
   IT IS THE MEMBER'S TO STATE AND NEVER THE MACHINE'S TO INFER. A predicate here — "is this parse the one
   whose scripts run?" — would be one fact answered from one place for every caller, and the two callers'
   answers come from different sections. */
typedef enum { FRAG_SCRIPTING_INERT = 0, FRAG_SCRIPTING_FRAGMENT = 1 } FragScriptingMode;

/* WHERE THIS MACHINE RESTS. The six members that parse markup are the same shape — a few leading steps of
   their own, then "let fragment be the result of invoking the fragment parsing algorithm steps", then either a
   placement or a return — so the stages after the entry belong to those two operations and each member's
   declaration names them in ITS OWN numbering. The clear is LAST in the enum on purpose: only the
   children-replacing members reach it, so insertAdjacentHTML and createContextualFragment simply do not
   declare that stage, and the driver's check is what says so if the shared machine ever reaches it from there.
   ONE LIST FOR ONE MACHINE. A stage of a shared machine is ONE rest point, so it carries ONE label naming
   every section that reaches it; which member a parked flow is in is what the declaration's `algorithm` says.
   Splitting the wording per member would be two statements of one stage, which is the drift the X-list exists
   to prevent. */
#define FRAG_STAGES(X) \
    X(FRAG_TRUSTED, "HTML §8.5.4 / §8.5.5 / §8.5.6 step 1 / §8.5.2 setHTMLUnsafe step 1 / §8.5.7 " \
                    "createContextualFragment step 1 (get trusted type compliant string with TrustedHTML and " \
                    "this sink), which is where Trusted Types §4.2's default-policy callback runs") \
    X(FRAG_START, "HTML §8.5.4 innerHTML setter steps 2-3 / §8.5.5 outerHTML setter steps 2-5 / §8.5.6 " \
                  "insertAdjacentHTML steps 2-4 / §8.5.7 createContextualFragment steps 2-6 / §8.5.2 " \
                  "setHTMLUnsafe step 2 and §8.6.4 \"Sanitization algorithms\"' set and filter HTML " \
                  "steps 1-6 (the target the fragment is parsed against)") \
    X(FRAG_FEED,  "HTML §8.5.4 step 4 / §8.5.5 step 6 / §8.5.6 step 5 / §8.5.7 step 7 / §8.6.4 " \
                  "\"Sanitization algorithms\"' set and filter HTML step 7 " \
                  "(the fragment parsing algorithm), one byte per step") \
    X(FRAG_PLACE, "HTML §8.5.4 step 5 / §8.5.5 step 7 / §8.5.6 step 6 / §8.6.4 \"Sanitization algorithms\"' set and filter HTML step 9 " \
                  "(insert one node of the fragment at the position the member names), and HTML §13.4 steps " \
                  "15-16 for the member that RETURNS it (§8.5.7): lexbor builds under §13.4 step 12's `root`, " \
                  "so this is where those nodes become the fragment's, one node per step") \
    X(FRAG_DONE,  "HTML §8.5.4 step 5 / §8.5.5 step 7 / §8.5.6 step 6 / §8.5.7 step 7 / §8.6.4 " \
                  "\"Sanitization algorithms\"' set and filter HTML step 9 (the fragment is placed or returned)")
/* FOUR STAGES, NOT FIVE, for insertAdjacentHTML and createContextualFragment: neither replaces its target's
   children, so FRAG_CLEAR is past the end of what they declare and the driver says so if the shared machine
   ever reaches it from there. It is its own list for that reason — the setter's declaration is the shared four
   followed by this one. */
#define FRAG_STAGE_CLEAR(X) \
    X(FRAG_CLEAR, "HTML §8.5.4 step 5 / §8.6.4 \"Sanitization algorithms\"' set and filter HTML step 9 (replace all within target: remove " \
                  "one existing child per step)")
/* AND §8.6.4 set and filter HTML's FILTER, whose stages are sanitizer.h's own X-list. They belong to the members that FILTER —
   §8.5.2's setHTML and setHTMLUnsafe — and are numbered after this machine's because the walk runs between the
   parse and the placement. The base travels to the walk as SAN_CHILD so neither file states the offset. */
enum { IDL_STEP_STAGE_BASE(FRAG_STAGES)
       FRAG_STAGES(JS_STEP_STAGE_ENUM) FRAG_STAGE_CLEAR(JS_STEP_STAGE_ENUM)
       SANITIZE_STAGES(JS_STEP_STAGE_ENUM) };

typedef struct {
    uint8_t where;
    uint8_t clear_first;          /* innerHTML= empties the element before parsing, one child per step */
    /* HTML §13.4's `allowDeclarativeShadowRoots`, which the two Unsafe members pass TRUE and every other
       fragment parse passes false. It rides the state because the step that acts on it — §13.2.6.4.4's
       template start tag, run over the finished fragment — is stages away from the one that read it. */
    uint8_t allow_declarative;
    /* §13.2.4.5's PARSER SCRIPTING MODE, one of the two values above, for the same reason
       `allow_declarative` rides here: the step that acts on it is §13.2.6.4.4's `script` start tag, run over
       the finished fragment at the FRAG_FEED boundary, and the member that chose it is stages behind. */
    uint8_t scripting;
    /* STEP 1's ANSWER, held across the stage boundary (owned). It is the compliant string and not the
       argument: once Trusted Types §3 exists, step 1 runs the default policy's callback and what the fragment
       is parsed from is what that callback RETURNED, which is a different value from the one passed in. */
    JSValue compliant;
    lxb_html_parser_t *parser;    /* THIS parse's own — see fragment_parser.c */
    char   *html;                 /* the markup, owned: the parser is handed slices of it across suspensions */
    size_t  len, off;
    lxb_dom_element_t *context;
    /* §8.5.5 STEP 5's / §8.5.7 STEP 6's `body`, when there is one: an element this machine CREATED to be the
       parse context and that is in no tree, so this machine has to destroy it. Owned, and released on the
       throw path too. */
    lxb_dom_element_t *own_context;
    lxb_dom_node_t *anchor, *ref, *frag, *node;
    /* §8.6.4 set and filter HTML's STEPS 4 AND 8. `san_config` is the canonical configuration the options resolved to, held from
       step 4 (which reads the options, before the parse) until step 8 hands it to the walk that consumes it;
       `sanitize` is whether this member filters at all, which is what tells §8.5.4's innerHTML setter,
       §8.5.6's insertAdjacentHTML and §8.5.7's createContextualFragment — none of which is
       `set and filter HTML` — from the three that are. */
    JSValue san_config;
    uint8_t sanitize;
    uint8_t safe;               /* §8.6.4 set and filter HTML's own `safe` argument — true for the two `setHTML` members */
    SanitizerWalk san;
} FragmentParse;

/* THE THREE HALVES OF THE OWNERSHIP DECLARATION a member puts on its IdlStepDecl. See core/idl_args.h for why
   `visit` is the ONE list and `release` is only what a declaration cannot name. */
void fragment_parse_visit(JSContext *ctx, void *st, JSStepVisit *v);
void fragment_parse_release(JSContext *ctx, void *st);
const char *fragment_parse_unforkable(const void *st);

/* Set the machine up for a parse of `html` into `where` around `anchor`, parsed in `context`'s tree-building
   context, under §13.2.4.5's `scripting` mode. `html` is COPIED because the JSString it came from is released
   before the first suspension. The member sets `hdr->stage = FRAG_FEED` and yields. */
void fragment_parse_begin(JSContext *ctx, FragmentParse *s, lxb_dom_element_t *context, lxb_dom_node_t *anchor,
                          int where, const char *html, bool clear_first, bool allow_declarative,
                          FragScriptingMode scripting);

/* §8.5.4 step 5 / §8.5.5 step 7 / §8.5.6 step 6 / §8.5.7 step 7 / §8.6.4 set and filter HTML step 9 — WHERE
   THE PARSE GOES NEXT. Exported because §8.6.4's FILTER resumes into it from the other side of the sanitizer
   walk, and the two must not drift. */
void fragment_parse_placement(FragmentParse *s, JSStepHdr *hdr);

/* ONE STEP of the parse-and-place. Returns JS_STEP_YIELD while there is more, or 0 when the fragment is in the
   tree. Every caller is a member body that returns whatever this returns. */
int fragment_parse_step(JSContext *ctx, JSStepHdr *hdr, FragmentParse *s);

#endif
