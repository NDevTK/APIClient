/* HTML §7.1.5 SANDBOXING — the flag set, its parse, and the two algorithms that produce one.
 *
 * IT IS NOT PART OF THE POLICY CONTAINER, AND FIVE SITES IN THIS TREE SAID IT WAS. §7.1.7's policy container
 * holds a CSP list, an embedder policy, a referrer policy and two integrity policies — no sandboxing flag set.
 * What a Document has is its own ACTIVE SANDBOXING FLAG SET, handed to it at creation: §7.3.2.1's create a new
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

/* §4.8.5 "The iframe element": "The supported tokens for sandbox's DOMTokenList are the allowed values defined
   in the sandbox attribute and supported by the user agent." BOTH HALVES ARE ANSWERED BY THE PARSE ITSELF and
   never by a second list of keywords standing beside it: an allowed value this component supports is exactly a
   token that RELAXES at least one flag, and a token the clauses above do not name cannot relax one. So a
   keyword added to §7.1.5's clauses becomes a supported token with nothing else to edit, and a keyword this
   component ignores reports itself unsupported instead of being claimed — which a hand-kept list of thirteen
   strings could not promise in either direction.
   `token` is ONE token, established by the caller (core/html/supported_tokens.c): a string carrying ASCII
   whitespace relaxes flags here while being no allowed VALUE at all. */
bool sandbox_keyword_supported(const char *token, size_t len);

/* §7.1.5's EMBEDDER, for determine-the-creation-sandboxing-flags: the element a navigable is nested THROUGH,
   reduced to the two sets that algorithm reads off it. */
typedef struct {
    SandboxFlags iframe_flags;     /* the embedder's IFRAME SANDBOXING FLAG SET (its `sandbox` attribute) */
    SandboxFlags document_flags;   /* the embedder's node document's ACTIVE SANDBOXING FLAG SET */
} SandboxEmbedder;

/* §7.1.5's DETERMINE THE CREATION SANDBOXING FLAGS for a browsing context, given `embedder` (NULL for §7.4's
   AUXILIARY navigable, which has none) and the browsing context's POPUP SANDBOXING FLAG SET. */
SandboxFlags sandbox_creation_flags(const SandboxEmbedder *embedder, SandboxFlags popup_flags);

/* §7.3.1.7 "Navigable target names"' RULES FOR CHOOSING A NAVIGABLE — the step that populates a new
   top-level browsing context's POPUP SANDBOXING FLAG SET: "If sandboxingFlagSet's sandbox propagates to
   auxiliary browsing contexts flag is set, then all the flags that are set in sandboxingFlagSet must be set in
   chosen's active browsing context's popup sandboxing flag set."
   NOT §7.1.5, WHICH IS WHERE THE FLAG IS DEFINED AND NOT WHERE IT IS APPLIED — the two sections are the
   vocabulary and the algorithm, and a bare §7.1 here named neither.
   `source_flags` is the OPENING document's active sandboxing flag set. This is what `allow-popups-to-escape-
   sandbox` turns off, and it is the reason a popup's flags are not simply its opener's. */
SandboxFlags sandbox_popup_flags(SandboxFlags source_flags);

/* The spec's name for ONE flag — for a `@WHY` that has to say WHICH. Aborts on a value that is not a single
   flag of §7.1.5's set, because a message naming "some flags" names nothing. */
const char *sandbox_flag_name(SandboxFlags one);

/* §7.1.5's SET AS TEXT — the ONE thing a flag set has to do that a `uint32_t` cannot, which is CROSS AN
 * INSTANCE. The header above says a set copies, parks and crosses "exactly as an integer does", and that is
 * true of the VALUE and false of the WIRE: an integer whose meaning is this build's declaration order is a
 * contract whose two ends agree only by accident, which is the same reason §7.1.4's three values cross as the
 * standard's own strings rather than as an enum's members. So what crosses is the SPEC'S OWN FLAG NAMES.
 *
 * THE SEPARATOR IS A COMMA AND IT IS NOT A CHOICE — the obvious answer is wrong here in a way that is worth
 * writing down, because §3.1.3's ancestor origins and §9.5's container answer both cross this same record
 * SPACE-joined and the reflex is to match them. §7.1.5's flag names CONTAIN SPACES ("sandboxed navigation
 * browsing context flag"), so a space-joined set does not parse at all: a reader cannot tell where one member
 * ends. The record itself splits on TAB, so that byte is spoken for too. Every one of the sixteen names is
 * lowercase ASCII letters, SPACE and (once) a '.', so a COMMA cannot occur inside one and is the separator
 * that leaves the set readable. It is checked rather than assumed — the serializer asserts it per name.
 *
 * THE EMPTY SET IS A WORD AND NEVER AN EMPTY FIELD. An unsandboxed navigable is the ordinary case, so the
 * field is empty far more often than not, and an empty field is exactly the hole a reader defaults past
 * (CLAUDE.md: a value the producer can legitimately omit is a POSITIVE statement the consumer reads as one).
 * `none` is that statement; it is not a flag name, so nothing can spell one by accident. */
#define SANDBOX_FLAGS_SERIALIZED_NONE "none"

/* Serialize `flags` into the form above. Returns an owned string, NULL on OOM — the caller CHECKs, because a
   dropped flag set is a peer created with an EMPTY active sandboxing flag set rather than one that
   failed. */
char *sandbox_flags_serialize(SandboxFlags flags);

/* And read one back. `false` for a token that names no §7.1.5 flag, for an empty field, and for a trailing or
   doubled separator — every one of which is a relay that stopped writing the field rather than a set with
   nothing in it, and the caller is the ABI entry that knows which record it came off. It answers a BOOL rather
   than aborting here for the reason `embedder_policy_value_of_token` does: the site that can name the record,
   the peer and the consequence is the entry, and an assert written in this file would name this file for every
   one of them. */
bool sandbox_flags_of_serialized(const char *text, SandboxFlags *out);

#endif
