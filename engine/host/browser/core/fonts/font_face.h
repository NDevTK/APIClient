/* CSS FONT LOADING §2 "The FontFace Interface" — THE OBJECT A PAGE CONSTRUCTS, AND NOTHING THAT LOADS.
 *
 * WHY THIS INTERFACE AND WHY NOW. A bundle that names a platform global this engine reaches on no global gets
 * a ReferenceError on the line that touches it, and every endpoint and every sink behind that line is lost —
 * which is a loss on this product's own thesis rather than a conformance nicety. Measured over a corpus of
 * real application bundles fetched at one instant, `FontFace` is the name whose absence costs the most: every
 * occurrence of it is a `new FontFace(` — an UNGUARDED use, in a position where absence RAISES — and NOT ONE
 * occurrence anywhere in that corpus is a feature detection. The derivation, never the figure, because a
 * corpus moves and a count of one rots:
 *     NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs --out <dir>
 *     node engine/absentrank.mjs --corpus <dir>/mirror --top 40     # read the THROWS band
 *     cd <dir>/mirror && grep -rlE 'new[[:space:]]+FontFace[[:space:]]*\(' .   # then OPEN each site
 *
 * WHAT THE SITES ACTUALLY READ, WHICH IS WHAT DECIDED THE SCOPE — read off the CALL SITES and never off §2's
 * member list, because a decomposition drawn from a spec installs an arm no page calls while leaving every
 * page dying one call earlier. Three sites, three bundles, and between them they touch: the CONSTRUCTOR with
 * a string source and with a `Uint8Array` source; `family`; `unicodeRange`, whose IDL DEFAULT `"U+0-10FFFF"`
 * one of them splits on a regular expression; and EIGHT descriptor attributes compared against a literal
 * object whose values are this dictionary's own defaults, verbatim. So the descriptors and their defaults are
 * not decoration — they are the observables the corpus reads, and they are written by §2.1's constructor.
 *
 * AND THAT IS THE TEST THIS COMPONENT HAD TO PASS BEFORE IT COULD BE LANDED AT ALL: WHICH ALGORITHM WRITES
 * THIS INTERFACE'S OBSERVABLES? For FontFace the answer is §2.1 "The Constructor", which is the part built
 * here — not the part deferred. An interface whose every observable is written by an algorithm a diff defers
 * is a shape-only install wearing a decomposition argument, and installing one is strictly WORSE than the
 * absence: a page's `if (window.X)` flips true, the branch behind it cannot complete, and the fallback branch
 * that was working is abandoned. THAT HAZARD IS MEASURED AWAY HERE RATHER THAN ARGUED AWAY: this corpus
 * carries ZERO guards on `FontFace` — no `typeof FontFace`, no `window.FontFace`, no `"FontFace" in window` —
 * so there is no guard for this install to flip and no fallback for it to abandon. What was absent was the
 * object the page goes straight at.
 *
 * WHAT IS NOT BUILT IS ABSENT AND NOT STUBBED, so a page that reaches for it gets the TypeError that names it
 * rather than a plausible datum. §2.2 "The load() method", the `loaded` attribute and the [[FontStatusPromise]]
 * slot behind them are the LOADING half and are named as a residual at the constructor. THE OTHER THREE THE
 * GAP AUDIT REPORTS ARE A DECISION AND NOT A GAP, said here so the row is not read as work owed: `features`,
 * `variations` and `palettes` come from a `partial interface FontFace` whose three types the standard itself
 * leaves open — `interface FontFaceFeatures` has NO members and carries the comment "The CSSWG is still
 * discussing what goes in here" — so installing them would be installing objects whose observables no
 * algorithm anywhere writes, which is the shape §NO STUBS forbids rather than the shape it asks for.
 * CSS Font Loading §3's FontFaceSet and its
 * §4's `FontFaceSource` — `document.fonts` — are a separate landing and are named at the foot of this header,
 * because the ORDER between them is a fact a reader needs and is not derivable from either file alone.
 *
 * ITS STATE IS A JS ARRAY ON THE OBJECT'S OWN SLOT AND NOT A C RECORD, which is core/html/element_internals.c's
 * reason at its own set: a mutation of an Array in an own property is a property write the per-flow COW delta
 * already captures, so two flows that each construct or re-describe a face do not write each other's, and the
 * snapshot machinery carries it across a park with nothing of this component's to serialize.
 *
 * THIS COMPONENT HAS NO FINALIZER AND NO gc_mark, and the reason is the one core/resize_observer/
 * resize_observer_size.c gives: the `JSClassDef` is a NAME and every other field is a zero, so the collector
 * has nothing of this file's to dispatch to — and nothing is held where the collector cannot see it either,
 * because the whole of a face's state is one Array in an OWN PROPERTY and the property walk in mark_children
 * and free_object is unconditional. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_FONT_FACE_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_FONT_FACE_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT — the class, the state-slot key and §2.1's constructor and eleven setter
   declarations. Released through core/platform.c's third column, so no host has a line to remember. */
void font_face_init(JSContext *ctx);
void font_face_free(JSRuntime *rt);

/* WEB IDL §3.7.3 "Interface prototype object" AND §3.8 "Platform objects implementing interfaces" FOR §2's
   INTERFACE, for one realm — reached through realm_declare_intrinsic, like every other per-realm install, so a
   realm this agent builds cannot be missing it. CSS Font Loading §2 declares no inherited interface, so
   browser/idl_inheritance.h carries `{ "FontFace", NULL, IDL_PROTO_OBJECT }` and the object is built over this
   realm's %Object.prototype%.
   THE TWO HALVES ARE ONE ENTRY BECAUSE THE INTERFACE IS `[Exposed=(Window,Worker)]`, and the argument is at the
   definition. This header used to declare a SECOND entry for the §3.8 half, which core/platform.c drove from
   its per-document install column — a column no WorkerGlobalScope realm reaches — so a worker realm built the
   prototype and never got the name. It is recorded rather than deleted because the split reads natural: the
   two halves ARE two algorithms, and nothing about a §3.8 entry says which column may call it. What decides
   that is the exposure set alone. */
void font_face_install_proto(JSContext *ctx);

/* Web IDL §3.7 Interfaces' implementation-check, for the one caller that will need it and does not exist yet:
   CSS Font Loading §3 "The FontFaceSet Interface"'s `FontFaceSet add(FontFace font)` declares an
   INTERFACE-typed argument, and idl_iface_brand wants the
   class. Exported now because the alternative is a second brand test written at that component — CLAUDE.md's
   two-right-answers-to-one-question — and because a predicate with no caller is a producer with no reader:
   this one's reader is named, is the next landing, and is stated in THE ORDER below rather than left to be
   inferred from a grep that will answer zero until that diff lands. */
bool font_face_is(JSValueConst v);

/* THE ORDER THE REST OF CSS FONT LOADING LANDS IN, AND WHY IT IS NOT THE ORDER §2, §3, §4 ARE PRINTED IN.
 *
 * It is stated HERE, at the piece that landed first, because a landing order is a claim about which member has
 * a CONSUMER today and that claim is only checkable from the outside — from the corpus, not from any of these
 * files. Each entry names the sites that make it survivable, so a reader can re-derive it rather than trust it.
 *
 *   (1) §2 + §2.1 — THIS FILE. Its consumer is every `new FontFace(` in the corpus; it needs nothing that does
 *       not exist. It flips no guard, because the corpus carries none on this name.
 *
 *   (2) §2.2 "The load() method", the `loaded` attribute and the [[FontStatusPromise]] slot behind them ARE
 *       NOT A LANDING OF THEIR OWN — they are members of the §3 landing below. This entry keeps its number,
 *       where a reader goes looking for them, because what made them (2) is re-derivable from the corpus and
 *       will be re-derived unless what refutes it stands beside it.
 *       IT SAID: "it is the only member of this standard left that FLIPS NOTHING: a corpus site reaches it on
 *       a face it constructed itself (`n.load().then(() => …add(n))`), unguarded, so an absent one already
 *       throws on the page's own line and a built one simply answers." The site is real and the quotation is
 *       exact. What is false is REACHES. The call and the face are in TWO methods: the one that mints the
 *       face opens `let a = t.fonts; for (let t of a) …` and ends `return a.add(s), …`, so the flow dies
 *       ITERATING `undefined` several statements earlier, in a function the quoted line does not name. The
 *       clause was read off the line it is ABOUT rather than off the line that REACHES it, which is the
 *       hop-by-hop trace that terminates at the first file holding what you need.
 *       AND THIS FILE ALREADY HELD THE REFUTATION, which is worth more than the incident: the WHAT THE SITES
 *       ACTUALLY READ paragraph above enumerates every member the corpus touches, and `load` is not among
 *       them. One file, two paragraphs, and the MEASURED one was right.
 *       THE DERIVATION, because a corpus moves and a count of one rots — and every hit is OPENED, since
 *       `.load()` is a name an application owns as readily as the platform does:
 *           NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs
 *           cd engine/.work/sitecorpus/mirror && grep -rloE '[.]load\(\)' .    # then OPEN each receiver
 *       Over one fetch of it THREE files answered and exactly ONE receiver was a FontFace; the others were an
 *       IndexedDB-backed library store and a passcode widget. A receiver-anchored `fontFace[s]?[.]load`
 *       pattern finds NONE of the three, the real one included — keying on the receiver carries no
 *       information about the member, which is why that channel reads clean here and is not.
 *       AND §3.2 SETTLES THE MERGE FROM THE STANDARD RATHER THAN FROM THE CORPUS, so it holds whatever a
 *       later fetch says: §3.2's own step is "For all of the font faces in the font face list, call their
 *       load() method", and the step beside it resolves "with the result of waiting for all of the
 *       [[FontStatusPromise]]s of each font face in the font face list, in order". §3.2 cannot be built
 *       without both, so they were never AFTER §3 — they are INSIDE it.
 *       WHAT §2.2 NEEDS THAT NOTHING ELSE IN THIS ORDER NAMES, because a member merged into a landing is a
 *       member whose own prerequisites go missing with it:
 *         — A PROMISE THAT CAN NEVER SETTLE IS WORSE THAN AN ABSENT MEMBER, which is (3)'s `ready` sentence
 *           owed to this slot. `load()` RETURNS [[FontStatusPromise]] without touching it on the arm §2.2
 *           states as "If font face's [[Urls]] slot is null, or its status attribute is anything other than
 *           "unloaded", return font face's [[FontStatusPromise]] and abort these steps" — so the slot must
 *           ALREADY settle for a BufferSource face and for a face §2.1's parse failed, or `load()` hands
 *           those two a promise nothing in this engine will ever settle, where today they get a TypeError
 *           that ends the flow with a name on it. Both settle paths are §2.1's own and are the two NAMED
 *           RESIDUALS in font_face.c; they land WITH §2.2, never after it.
 *         — §2.2's FETCH ARM IS STATED OVER THE PARSED `src`: "Using the value of font face's [[Urls]] slot,
 *           attempt to load a font as defined in [CSS-FONTS-3], as if it was the value of a @font-face rule's
 *           src descriptor." THIS CLAUSE USED TO SAY THAT DESCRIPTOR HAS NO VALUE GRAMMAR HERE and that the
 *           slot therefore holds the RAW string, "because core/css/ does not type this descriptor and the
 *           collector keeps an untyped one verbatim". It is REWRITTEN RATHER THAN DELETED because the
 *           reasoning was exactly right and a reader meeting an untyped descriptor will re-derive it:
 *           core/css/css_font_src.h is css-fonts-4 §4.3's grammar, reached from the same
 *           `CSSOM_BLOCK_FONT_FACE` seam every other descriptor goes through, so a `src` now arrives PARSED
 *           and SERIALIZED and a value outside §4.3.1 is §2.1's "fail to parse correctly" like any other.
 *           WHAT IS STILL TRUE AND IS THE PART (2) DEPENDS ON: the parsed LIST is internal to that component
 *           — it exports the serialization and not the items — and no CSS url is fetched anywhere in this
 *           engine, which the subresource seam's own callers say, both of them ELEMENTS rather than values:
 *               git grep -n 'engine_pending_resource_url' engine/host/browser/core/
 *           So §2.2 needs the ITEMS entry css_font_src.h names as its first residual, and it needs a fetch;
 *           it no longer needs a grammar.
 *         — AND THE BASE URL IS AN OPEN ISSUE IN THE STANDARD ITSELF rather than a gap here, so a builder
 *           who expects to find the answer by reading harder will not: §2.1 carries "Need to define the base
 *           url, so relative urls can resolve. Should it be the url of the document? Is that correct for
 *           workers too, or should they use their worker url? Is that always defined?"
 *       RETIREMENT: this entry goes when (3) has landed, because it is (3)'s MEMBERSHIP that it corrects and
 *       the correction is spent with it.
 *
 *   (3) §3 "The FontFaceSet Interface", §4 "The FontFaceSource Mixin"'s `Document.fonts`, §3.2 "The load()
 *       method" and §3.3 "The check() method" — WHICH IS ONE LANDING, AND THE SPLIT THAT STOOD HERE IS THE
 *       DEFECT THIS ENTRY EXISTS TO RECORD. §3-with-§4 was (2) and the two font-matching members were (3), on
 *       the ground that a site landing (2) alone "dies at the `check`, one call further on than it dies
 *       today, which is progress". Both halves of that are wrong, and they are wrong for one reason:
 *       `document.fonts` IS A SURFACE GUARD, so its TRUE branch is not one member but every member the page
 *       goes on to read off the set.
 *         — A guarded site's true branch CALLS `load()`: `` if (!(`fonts` in document)) return
 *           Promise.resolve(); … document.fonts.load(font, text) ``, on a real messenger's boot line,
 *           immediately before the dynamic `import()` of its bootstrap chunk. Today the guard reads false and
 *           the early return is taken. Install the set without `load()` and the guard flips, `undefined(…)`
 *           throws SYNCHRONOUSLY out of a non-async function, and the import — with the thirty-odd chunks its
 *           dep list names — never happens. That is §NO STUBS' hazard exactly: the branch behind the guard
 *           cannot complete AND the branch that was working is abandoned, so BOTH arms are worse than the
 *           state they replaced.
 *         — The UNGUARDED site the split was justified by does not move at all. It reads
 *           `x.fonts.check(font, text) || await x.fonts.load(font, text)` with `x` its own
 *           `ownerDocument ?? document`, so today it throws reading `.check` off `undefined` and after a
 *           set-without-`check` it throws CALLING `undefined` — the same flow, the same line, one token
 *           later. One token is not one call and is not progress.
 *       SO THE RULE, WHICH IS WHAT SURVIVES ANY CORPUS: where a surface is reached through a GUARDED
 *       ACCESSOR, the landing unit is the accessor PLUS every member the guarded branch calls — never the
 *       accessor alone — because there is no install of the accessor that does not flip the guard.
 *       AND `ready` IS OWED ITS FULFILLING CONDITION OR IS DELIBERATELY ABSENT. `await undefined` yields and
 *       resumes and `x?.ready && x.ready.then(…)` skips, so an ABSENT `ready` is survivable; a PERMANENTLY
 *       PENDING one is not, because a promise created pending and never fulfilled hangs that flow for ever.
 *       §3's [[ReadyPromise]] is fulfilled by "switch the FontFaceSet to loaded", which exits early while the
 *       set is PENDING ON THE ENVIRONMENT — "the document is still loading", "pending stylesheet requests",
 *       "pending layout operations" — so the condition is what makes `ready` buildable at all.
 *       WHICH MEMBERS THE GUARDED BRANCH ACTUALLY CALLS IS A FACT ABOUT A CORPUS AND ABOUT ONE FETCH OF IT, so
 *       it is a command and never a list. KEY ON THE MEMBER AND NEVER ON THE RECEIVER, which is the way both
 *       readings before this one went short: a `document\.fonts` pattern misses `i.fonts`, `x.fonts` and
 *       `contentDocument.fonts`, and those are where `check`, `add` and `has` live.
 *           NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs
 *           cd engine/.work/sitecorpus/mirror
 *           for m in check load ready add delete clear forEach status size size_NO_SUCH_MEMBER
 *           do printf '%-9s ' "$m"; grep -rlE "[.]fonts[.]$m[^A-Za-z0-9_]" . | cut -d/ -f2 | sort -u | tr '\n' ' '
 *              echo; done
 *           grep -rlE "fonts[\"'\`][[:space:]]*in[[:space:]]" .      # the SURFACE guards
 *       THE TRAILING `[^A-Za-z0-9_]` IS NOT TIDINESS AND THE INVENTED MEMBER IS NOT CEREMONY. `fonts` is a
 *       field name an application owns as readily as the platform does — one corpus site's own font manager
 *       answers `.fonts.loadRequiredFontsForCurrentPage`, which an unanchored `[.]fonts[.]load` claims for
 *       §3.2 — so this channel FALSE-POSITIVES as well as under-reads, and every hit is OPENED rather than
 *       counted. The invented member is what says a zero is about the corpus rather than about the grep.
 *       RETIREMENT: this entry goes when §3, §4, §3.2 and §3.3 have landed together, because the ordering
 *       claim is then spent.
 *
 *   (4) §4.2 "Interaction with CSS's @font-face Rule" — the CSS-CONNECTED faces. Until it lands a document's
 *       set starts EMPTY, which is a narrower answer than a browser's for a document that declares
 *       `@font-face` rules and is the reason §3's `add` cannot yet reach its InvalidModificationError arm.
 *
 * AND (0) IS A CSS-COMPONENT GAP RATHER THAN A FONT-LOADING ONE, which is where a reader building (1) will
 * meet it: §2.1 parses each descriptor "according to the grammars of the corresponding descriptors of the CSS
 * @font-face rule", and core/css/ types some of those grammars and not others. What an untyped one does here
 * is stated at the parse in font_face.c, with the command that says which is which. */

#endif /* ENGINE_HOST_BROWSER_CORE_FONTS_FONT_FACE_H */
