/* HTML §7.1.5 SANDBOXING — the flag set, its parse, and the two algorithms that produce one.
 *
 * IT IS NOT PART OF THE POLICY CONTAINER, AND FIVE SITES IN THIS TREE SAID IT WAS. §7.1.7's policy container
 * holds a CSP list, an embedder policy, a referrer policy and two integrity policies — no sandboxing flag set.
 * What a Document has is its own ACTIVE SANDBOXING FLAG SET, handed to it at creation: §7.2's create a new
 * browsing context and document gives the initial about:blank the CREATION SANDBOXING FLAGS, and §7.5.1's
 * create and initialize a Document object gives a navigated document navigationParams's FINAL SANDBOXING FLAG
 * SET, which §7.4.5 builds as "the union of targetSnapshotParams's sandboxing flags and policyContainer's CSP
 * list's CSP-derived sandboxing flags". So the container contributes ONE of the two inputs, through the CSP
 * `sandbox` directive alone (policy_container.h), and the other comes from the navigable's container element
 * and its embedder's own flags. A reader who follows the old sentence looks for a field that cannot exist.
 *
 * A FLAG SET IS A VALUE, WHICH IS WHY IT TIME-TRAVELS FOR FREE. Every flag here is a bit of one word, so the
 * set copies, crosses an instance and parks with a snapshot exactly as an integer does — there is no graph to
 * serialize and no pointer that could name another flow's memory. The per-flow question is answered by its
 * INPUTS rather than by capturing its output: an `<iframe sandbox>`'s flag set is parsed from a DOM attribute,
 * whose write the COW delta already captures, and the child navigable a flow creates is that flow's, so two
 * flows that disagree about the attribute create two children with two flag sets and neither can see the
 * other's. The set is then IMMUTABLE for the life of the Document that holds it — §7.1.5 says it is populated
 * by the navigation algorithm and nothing else ever writes it — so a capture of the output would be a delta
 * entry for a field with no second write. What replaces a navigable's flag set is a NAVIGATION, and a
 * navigation replaces the whole Document; core/dom/document.c's install asserts on the second one.
 *
 * THE NAMES ARE THE SPEC'S, ONE STRING PER FLAG, so a `@WHY` that names a flag names the sentence to read.
 * A flag the spec defines and this file did not carry would be a silent `false` at every site that asks for
 * it, which is the shape the five placeholder predicates had; the whole set is here for that reason. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_SANDBOXING_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_SANDBOXING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* §7.1.5's SET, in the order the standard lists them. The text is the spec's own name for the flag. */
#define SANDBOX_FLAG_LIST(X)                                                                                 \
    X(SANDBOX_NAVIGATION,                  "sandboxed navigation browsing context flag")                     \
    X(SANDBOX_AUXILIARY_NAVIGATION,        "sandboxed auxiliary navigation browsing context flag")            \
    X(SANDBOX_TOP_LEVEL_NAVIGATION_WITHOUT_USER_ACTIVATION,                                                  \
      "sandboxed top-level navigation without user activation browsing context flag")                        \
    X(SANDBOX_TOP_LEVEL_NAVIGATION_WITH_USER_ACTIVATION,                                                     \
      "sandboxed top-level navigation with user activation browsing context flag")                           \
    X(SANDBOX_ORIGIN,                      "sandboxed origin browsing context flag")                          \
    X(SANDBOX_FORMS,                       "sandboxed forms browsing context flag")                           \
    X(SANDBOX_POINTER_LOCK,                "sandboxed pointer lock browsing context flag")                    \
    X(SANDBOX_SCRIPTS,                     "sandboxed scripts browsing context flag")                         \
    X(SANDBOX_AUTOMATIC_FEATURES,          "sandboxed automatic features browsing context flag")              \
    X(SANDBOX_DOCUMENT_DOMAIN,             "sandboxed document.domain browsing context flag")                 \
    X(SANDBOX_PROPAGATES_TO_AUXILIARY,     "sandbox propagates to auxiliary browsing contexts flag")          \
    X(SANDBOX_MODALS,                      "sandboxed modals flag")                                           \
    X(SANDBOX_ORIENTATION_LOCK,            "sandboxed orientation lock browsing context flag")                \
    X(SANDBOX_PRESENTATION,                "sandboxed presentation browsing context flag")                    \
    X(SANDBOX_DOWNLOADS,                   "sandboxed downloads browsing context flag")                       \
    X(SANDBOX_CUSTOM_PROTOCOLS_NAVIGATION, "sandboxed custom protocols navigation browsing context flag")

#define SANDBOX_FLAG_BIT_(name, text) name##_BIT_,
enum { SANDBOX_FLAG_LIST(SANDBOX_FLAG_BIT_) SANDBOX_FLAG_COUNT_ };
#undef SANDBOX_FLAG_BIT_

#define SANDBOX_FLAG_VALUE_(name, text) name = 1u << name##_BIT_,
enum { SANDBOX_FLAG_LIST(SANDBOX_FLAG_VALUE_) };
#undef SANDBOX_FLAG_VALUE_

/* A SANDBOXING FLAG SET. One word, because every one of §7.1.5's flags is one bit and a set of them is their
   union — which is also what makes "the union of the flags present in the following sets" an `|`. */
typedef uint32_t SandboxFlags;

/* §7.1.5's PARSE A SANDBOXING DIRECTIVE, given the value of an `<iframe sandbox>` attribute or of a CSP
   `sandbox` directive. Note what it returns for the EMPTY input: `sandbox=""` is the most restrictive form
   there is, so an empty token list yields nearly the whole set rather than nothing. */
SandboxFlags sandbox_parse_directive(const char *input, size_t len);

/* §7.1.5's EMBEDDER, for determine-the-creation-sandboxing-flags: the element a navigable is nested THROUGH,
   reduced to the two sets that algorithm reads off it. */
typedef struct {
    SandboxFlags iframe_flags;     /* the embedder's IFRAME SANDBOXING FLAG SET (its `sandbox` attribute) */
    SandboxFlags document_flags;   /* the embedder's node document's ACTIVE SANDBOXING FLAG SET */
} SandboxEmbedder;

/* §7.1.5's DETERMINE THE CREATION SANDBOXING FLAGS for a browsing context, given `embedder` (NULL for §7.4's
   AUXILIARY navigable, which has none) and the browsing context's POPUP SANDBOXING FLAG SET. */
SandboxFlags sandbox_creation_flags(const SandboxEmbedder *embedder, SandboxFlags popup_flags);

/* §7.1's RULES FOR CHOOSING A NAVIGABLE, the step that populates a new top-level browsing context's POPUP
   SANDBOXING FLAG SET: "if sandboxingFlagSet's sandbox propagates to auxiliary browsing contexts flag is set,
   then all the flags that are set in sandboxingFlagSet must be set in chosen's popup sandboxing flag set."
   `source_flags` is the OPENING document's active sandboxing flag set. This is what `allow-popups-to-escape-
   sandbox` turns off, and it is the reason a popup's flags are not simply its opener's. */
SandboxFlags sandbox_popup_flags(SandboxFlags source_flags);

/* The spec's name for ONE flag — for a `@WHY` that has to say WHICH. Aborts on a value that is not a single
   flag of §7.1.5's set, because a message naming "some flags" names nothing. */
const char *sandbox_flag_name(SandboxFlags one);

#endif
