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
 *   (2) §3 "The FontFaceSet Interface" TOGETHER WITH §4 "The FontFaceSource Mixin"'s `Document.fonts`. They are
 *       ONE landing and not two: `setlike<FontFace>` and `add(FontFace font)` have no meaning without a
 *       FontFaceSet to be reached through, and `document.fonts` with no set behind it is a member whose type
 *       does not exist. Its consumers are the sites that iterate `document.fonts` and then `add` a face to it.
 *       IT IS THE LANDING THAT CARRIES THE ONE MEASURED GUARD IN THIS SURFACE — a corpus site spelled
 *       `` if (`fonts` in document) try { await document.fonts.ready } catch {} `` — so it is the one that
 *       must arrive with that guard's TRUE branch survivable. `await undefined` yields and resumes, so an
 *       ABSENT `ready` is survivable and a PERMANENTLY PENDING `ready` is not: a promise created pending and
 *       never fulfilled hangs that flow for ever, which is strictly worse than the absence it replaced. §3's
 *       [[ReadyPromise]] is fulfilled by "switch the FontFaceSet to loaded", which exits early while the set is
 *       PENDING ON THE ENVIRONMENT — "the document is still loading", "pending stylesheet requests", "pending
 *       layout operations" — so `ready` is owed that condition or it is owed nothing at all.
 *
 *   (3) §2.2 "The load() method", §3.2 "The load() method" and §3.3 "The check() method", which is the FONT
 *       MATCHING half: §3.5 "Interaction with CSS Font Loading and Matching". A corpus site calls
 *       `document.fonts.check(font, text)` and `document.fonts.load(font, text)`, and until this lands that
 *       site dies at the `check` — one call further on than it dies today, which is progress and is not
 *       completion, and saying which of the two it is belongs here rather than in a report.
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
