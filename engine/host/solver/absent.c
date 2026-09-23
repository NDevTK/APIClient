/* WHICH ABSENT READ IS INPUT, AND WHICH IS A COMPONENT THIS ENGINE OWES.
 *
 * Both wear the same shape — a name that resolves nowhere — and answering them the same way loses either the
 * whole logged-in surface or every forcing function.
 *
 * SERVER-INJECTED APP STATE is unknown INPUT. `window.__FLAGS`, `__USER`, `gon` are written into the document
 * by the server for a logged-in visitor and simply absent for this one, so what they hold is not `undefined` —
 * it is UNKNOWN. Answering `undefined` makes `__FLAGS.admin` throw on the first field access and buries every
 * endpoint behind it, which is precisely the surface this tool exists to reach: the bundle ships the auth and
 * admin code to a logged-out visitor and it never runs. Symbolic instead, so the gate FORKS and the logged-in
 * arm is explored.
 *
 * A WEB API THIS ENGINE HAS NOT BUILT is honestly absent. Its ReferenceError is the forcing function that names
 * the component to write, and handing back a symbol instead would let a flow run past a missing capability and
 * report a surface it never reached.
 *
 * THE DISTINCTION IS THE STANDARD'S TO MAKE, NOT A LIST'S. A global name belongs to the platform exactly when
 * Web IDL exposes it on Window, and browser/platform_names.h is that set, generated from @webref/idl by
 * engine/idlgen.mjs. It replaced a 22-name list typed into main.c — and the difference is not cosmetic: every
 * interface off that list (Node, Element, Event, DOMException, HTMLElement, and ~1300 more) was mistaken for
 * app state, so a branch on one FORKED instead of throwing. A page touching eight of them multiplied the
 * frontier by 256; a WPT document exhausted 2.8 GB in forty seconds doing it. A hand-maintained allowlist
 * cannot be right about a surface of this size, and the moment it is wrong the error is silent.
 *
 * AND THE LESSON RECORDED THERE WAS HALF OF IT, WHICH IS WHY THE SAME DEFECT WAS STILL HERE AFTERWARDS UNDER
 * A DIFFERENT VOCABULARY. That entry reads as being about where the SET came from — typed versus generated —
 * and the generation was never the whole of the fix, because the ANSWER SHAPE of this arm is a SUPPRESSION
 * over a DEFAULT: a name the suppression does not cover is app state, so whatever the suppression is complete
 * FOR decides nothing about the population that escapes it. platform_names.h is complete for Web IDL and was
 * EMPTY for ECMAScript, and THREE standards put names on the global object. So every §19 name a build does not
 * install — which names those are is a fact about the build and never about this file — fell through to "app
 * state", and a feature-detect on one minted an unknown and FORKED where a real browser answers `undefined`
 * and where this engine owes a component exactly as it owes an unbuilt interface. The second vocabulary is
 * browser/language_names.h, derived by engine/esglobalgen.mjs from the §19 subclauses of the ECMAScript
 * section index this tree already commits for citegen.mjs — the same rule as the first, applied to the other
 * standard, and derived from that standard's own artifact rather than typed here. The THIRD is
 * browser/i18n_names.h, ECMA-402 §8 The Intl Object, derived by engine/i18nglobalgen.mjs — and it arrived by
 * the same route as the second: the name was in neither table, so it defaulted to app state, and 259
 * references across 13 of this project's 18 committed mirrors minted unknowns whose gates forked. THAT THE
 * SAME DEFECT RECURRED TWICE AFTER BEING FIXED ONCE IS THE FACT WORTH KEEPING, and its shape is the one this
 * file's own header already names: an answer that is a SUPPRESSION over a DEFAULT is wrong about exactly the
 * population its suppression does not cover, so completing one vocabulary says nothing about the next.
 * A fourth standard would arrive the same way and be invisible the same way.
 *
 * WHAT THAT DOES NOT FIX, STATED PLAINLY SO THE NEXT READER DOES NOT MISTAKE IT FOR THE WHOLE ANSWER: the
 * default is still a default. A name is answered as server-injected app state because NO vocabulary claimed
 * it, which is a negative, while the record arm below rests on a POSITIVE fact (the engine granted the
 * container's extent to the document's own bytes). The population that escapes both vocabularies is every
 * other host's globals and every library's own store — `process`, `QObject`, `setImmediate`,
 * `__core-js_shared__`, `_sentryDebugIds`, `webpackJsonp` — and at the instant of the read this file holds
 * NOTHING that tells one of those from a `window.__FLAGS` the server would have written for a logged-in
 * visitor: both are "not a standard's name, never written by anything, missed on the global". Erring toward
 * the fork is STILL the correct side to be wrong on — a concrete `undefined` buries the admin code, which is
 * the loss this file exists to prevent — so this arm stays as it is until something can DECIDE it rather than
 * default it.
 *
 * WHAT IS DELETED FROM THAT SENTENCE IS ITS SECOND HALF, WHICH PRICED THE DEFAULT BY CITING THE WFQ: "the WFQ
 * starves an arm that emits nothing". That is a primitive cited as precedent with none of its preconditions
 * named — the move §CONCRETIZE-ON-PIN carries its own annotation for, arriving here over a different
 * primitive — and the precondition this one needs is one this arm denies BY CONSTRUCTION.
 *
 * THE MECHANISM CITED STARVES A RUNNER AND WHAT IT WAS CITED AGAINST IS A POPULATION, AND THOSE ARE NOT THE
 * SAME OBJECT. Aging charges the thread a MEMBER actually burned, and a fork COPIES that charge onto the arm
 * — `cpu` with its `cpu_gen`, and `visits` with it — because §ONE-WFQ requires a fork to be RANK-NEUTRAL. So
 * the flow that keeps holding the thread does sink below the siblings it left behind, and that is why the two
 * per-position chains making this same citation — the unknown `length` and the unknown own-key set, each
 * asking "is there one beyond n?" — are RIGHT to make it: each is LINEAR, one continuing flow per position,
 * so the runner is exactly the object aging reaches. A BRANCHING world is not that object. Its N members
 * share the charge accrued by whichever ONE of them held the thread, so what the arm owes as a whole does not
 * grow with N — and its non-branching sibling, which never runs, sits frozen at the identical charge it was
 * born with. The reward term cannot break the tie either, and not because it is small: it is held per
 * ACCOUNT, so on the one-family frontier a document actually has (flow.h: "`families: 1` is the identity and
 * the term is structurally an offset") it is a common offset that cancels out of every comparison INSIDE the
 * document — which is precisely the comparison the deleted clause claimed it decided. Every separating term
 * is therefore common to the two sides of this branch, and "outranked" names no relation between them.
 *
 * MEASURED, AND THE SHAPE RATHER THAN THE TOTALS IS WHAT TRAVELS (one 91s budget, gitlab.com/explore, at the
 * artifact built from 2fd95568 — an ancestor of this commit, so re-derive before quoting): 29351 of 29388
 * frontier members stood at ONE program cursor, and ALL SIXTY-FOUR predicate rows of that run's `_forkAt`
 * decoded to derivations of ONE name missed on the global — `webpackJsonp`, read as the right-hand side of
 * webpack's own `this.webpackJsonp = this.webpackJsonp || []` in the first program the page runs. Six
 * structurally different predicates over derivations of that one name, composing multiplicatively inside the
 * callback the truthy world hands an unknown to. The individual chains were starved exactly as promised
 * (`LengthOfArrayLike>0` 2761 hits, `>1` 28, `>2` absent). Their PRODUCT was not, and no term reads a
 * product. The document emitted four endpoints in ninety-one seconds.
 *
 * AND THAT CENSUS IS SATURATED ON A REAL PAGE, WHICH IS ONE CLAUSE MORE THAN THE PARAGRAPH ABOVE SAYS.
 * "All sixty-four predicate rows decoded to" one name is true of the rows the table NAMED, and a reader takes
 * it for a complete accounting. Re-measured at a later artifact on the same document: error bound 59..74 with
 * the OVERFLOW row LEADING at 53..98% of the mass — and decide.c states that reading itself, an overflow row
 * that leads being "the table saying it has not answered". So the IDENTITY of the top rows travels and their
 * SHARE does not, which is the distinction the paragraph above draws and then stops one clause short of.
 *
 * AND THE FIXTURE AND A REAL PAGE FAN THROUGH DIFFERENT HOOKS, WHICH DECIDES WHERE A REPAIR MAY BE AIMED.
 * The smoke fixture's mass is `present`: its top four rows are members an inline program ASSIGNED six lines
 * above the comparison, so a value is in hand. A real page's mass is `absent` — `webpackJsonp` read where
 * nothing has defined it. Measured at ONE artifact, same build for both sides: the fixture's named rows carry
 * 16 distinct sources and its top four are inline-record members; the real bundle's carry ONE, `webpackJsonp`,
 * in all four censuses, with no inline-record row in any of them. SO A NARROWING OF `present` WOULD RETIRE THE
 * FIXTURE'S TOP FOUR AND TOUCH NOTHING OF A REAL PAGE'S FAN. The subproblems recorded below at
 * `absent_present_hook` are aimed at `present`, so that is the reading they must be weighed against before
 * one is built: the shape they describe IS in the real document (`window.gon={}`, `gl = window.gl || {}`) and
 * contributes no named row, and bound-aware any such site the table does not hold took <= 59..74 forks
 * against `webpackJsonp`'s 1458..3074.
 *
 * SO THE HONEST STATEMENT OF WHAT WOULD HAVE TO EXIST IS ABOUT THE ACCOUNTING UNIT AND NOT ABOUT A WEIGHT,
 * AND THIS FILE IS NOT THE PLACE IT WOULD BE BUILT. flow.h's own T/P row says of exactly this reading — the
 * thread reaching a fresh member nearly every time — that "no term of flow_weight reaches it, and a weight
 * change made against this reading fixes nothing and can only make the order worse", and §ONE-WFQ's
 * fork-neutrality is what forbids the obvious candidates: any term that told the two sides of THIS branch
 * apart is a term a fork does not carry. What must exist afterward is a frontier on which the two sides of an
 * example-free branch are COMPARABLE AT ALL, which is a question about what mints an ACCOUNT and not about
 * what a weight reads. Its absence shows exactly as measured: a document whose whole frontier descends from
 * one absent name, spending its whole budget, while every term that could order it stays common to both
 * sides of the branch that made it.
 *
 * AND THE THING THIS PARAGRAPH USED TO NOMINATE AS THAT DECIDER IS UNSOUND — IT IS WRITTEN OUT HERE BECAUSE IT
 * WAS LANDED AS AN INSTRUCTION AND A LANE WOULD HAVE BUILT IT. It said the classification is decidable from a
 * fact that arrives AFTER the read — the program's OWN later definition of the same global name, `x || (x =
 * {})` being the canonical shape — so the arm asserting an external supplier could be retired once the
 * family established that the program defines the name. The counterexample is one line long and it is one of
 * the commonest lines on the web: `window.__FLAGS = window.__FLAGS || {}`. Every SSR hydration shim, `gon`,
 * and `window.dataLayer = window.dataLayer || []` is written that way. The program DOES define the name, so
 * the retirement fires, and the surviving arm holds a bundle-built `{}` whose extent no grant admits — so
 * `__FLAGS.admin` answers `undefined` concretely and the admin surface is buried. That is the exact loss this
 * file exists to prevent, delivered by the mechanism proposed to improve it.
 *
 * THE GENERAL FORM, WHICH IS WHAT SURVIVES AND IS WORTH MORE THAN THE INCIDENT: EVERY CANDIDATE THAT KEYS ON
 * THE PROGRAM'S OWN HANDLING OF ABSENCE FAILS, AND FAILS FOR ONE REASON. A defensive read (`window.__USER &&
 * …`), a `typeof` guard, and a defining write (`x = x || {}`) are all the program saying IT DOES NOT KNOW
 * WHETHER THE NAME IS BOUND — which is equally true of a polyfill probing for another host's global and of a
 * bundle reading state its server may not have rendered. The program's code is written by people who do not
 * know, in EITHER case, so nothing in that code can separate the two. Only a fact about the PRODUCER can, and
 * this engine holds one for a record (js_document_container_grant asks whether the document's own bytes wrote
 * the extent) and holds none for a name nothing wrote. The honest statement of what would have to exist is
 * therefore not a run fact about the program at all: it is a SECOND OBSERVATION OF THE DOCUMENT — the same
 * address fetched under different credentials, whose injected namespace differs exactly in the names a server
 * renders for a session — which is a differential this tool could take and does not.
 *
 * AND THE PRIMITIVE THIS WAS ANCHORED TO IS NOT THE ONE IT LOOKS LIKE. §solver's CONCRETIZE-ON-PIN is a
 * DETERMINATION about one value by a predicate THE FLOW ITSELF EVALUATED — concolic_new states it exactly:
 * "A pin is a fact about THIS value, so it applies exactly where the value being minted IS the pinned one."
 * A retraction is the opposite twice over: the fact is established by a SIBLING arm rather than by the flow
 * being narrowed, and it deletes a world instead of determining a value. concolic_pin already names that
 * hazard about this very primitive — "the witness does not merely get emitted, it deletes the sibling world
 * the run never contradicted. An absent pin forks and explores both; a wrong one decides an arm nothing
 * downstream can contradict." The retraction is that wrong pin, one level up. And whatever a later reader
 * decides here, it is not a §NO BOUNDS question: this arm has no cap in it and must not gain one — the cost
 * of a world nobody can contradict is the WFQ's to order, never this file's to refuse.
 *
 * AND IT IS ASKED OF A PRESENT MEMBER AS WELL AS OF A MISSING ONE, which is the half that decides the case
 * §solver names by name. A server that ships `window.__FLAGS={admin:false}` has WRITTEN the field, so the read
 * hook below never sees it — the engine asks that one only where the prototype chain ran out. Answering
 * `false` from the slot then decides `if (__FLAGS.admin)` for the whole program and buries the admin surface,
 * which is the identical loss this file exists to prevent, reached through a slot instead of a hole. The
 * extent of that record was the SERVER'S choice against this visitor's credentials, so what it holds is a
 * per-session fact and not a program constant: unknown for control flow, and — unlike a missing member —
 * carrying the bytes the server actually sent as its EXAMPLE. §solver: "a loaded `features.admin:false` must
 * NOT concretize the gate, or the admin endpoint is lost — config is opaque-for-control-flow yet carries its
 * loaded value as the example."
 * The two halves share ONE path composition and ONE registry, which is why they are one file: a member's
 * provenance is `gon.current_user_id` whether or not the record holds it, and two spellers would be two names
 * for one unknown the moment either drifted.
 *
 * AND THE QUESTION IS ASKED OF A PRESENT PARENT AS OFTEN AS OF A MISSING GLOBAL, which is the half this file
 * did not have. A server does not only decline to write `window.__FLAGS`; far more often it writes
 * `window.gon={}` and then the two of the twenty-three fields the bundle reads that THIS visitor is entitled
 * to. Every one of the other twenty-one missed on a present object and answered `undefined`, so
 * `if (!gon.current_user_id) return null` never forked and the logged-in surface stayed buried behind a rule
 * that was written for it. The engine decides WHICH records those are and what this file owes is the PATH each
 * one is read by, because a member's identity is `gon.current_user_id` and never a bare `current_user_id` that
 * a second namespace's identically-named field would be indistinguishable from.
 *
 * AND THE ENGINE'S RULE IS WORTH STATING HERE, BECAUSE IT IS THE ONE THING THIS FILE MAY NOT SECOND-GUESS AND
 * THE ONE THING THAT HAS BEEN WRONG TWICE. A container is on this channel iff THE DOCUMENT'S OWN BYTES WROTE
 * ITS EXTENT — an object or array literal, or a `JSON.parse` in an inline `<script>`, granted at the operation
 * that writes the member list (js_document_container_grant), never inferred from how or when the object was
 * allocated. A LIST is such a container for the purpose of being DESCENDED THROUGH and is never a record whose
 * miss is the server's silence (§10.4.2 Array Exotic Objects: the list states its own length), so this file
 * sees a list only as a path component — `__STATE__.users[0]` — and never as a base it is asked about. The
 * two halves of that are what keep the channel honest in the two directions it can fail. A record wrongly ON
 * it answers a member that HAS a real answer with an unknown, and nothing throws: a PLATFORM OBJECT is the
 * case, because Web IDL §3.8 "Platform objects implementing interfaces" makes its member list the INTERFACE'S
 * and not the document's, so an `Event` an inline script leaves in a `var` must answer `undefined` for a
 * member it does not have — and a member of an interface this engine has not BUILT yet must be honestly
 * absent, since that absence is the forcing function naming the component to write. A record wrongly OFF it
 * loses a fork, which is loud and recoverable. So the engine grants narrowly and this file never widens: a
 * base it is asked about is one the engine has already decided, and every DCHECK below says so.
 *
 * THE REGISTRY HOLDS NO REFERENCE, and that is sound rather than lucky: a row is only ever consulted for an
 * object the ENGINE has marked as published, the mark is cleared at every allocation, and the only thing that
 * sets it is the publication that files the row. So a recycled address cannot answer with a dead document's
 * path — it has no mark until something publishes it, and publishing appends the row that describes it. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/absent.h"
#include "solver/concolic.h"
#include "browser/i18n_names.h"
#include "browser/language_names.h"
#include "browser/platform_names.h"

#define PLATFORM_NAMES_N ((int)(sizeof(PLATFORM_NAMES) / sizeof(PLATFORM_NAMES[0])))
#define LANGUAGE_NAMES_N ((int)(sizeof(LANGUAGE_NAMES) / sizeof(LANGUAGE_NAMES[0])))
#define I18N_NAMES_N ((int)(sizeof(I18N_NAMES) / sizeof(I18N_NAMES[0])))

/* MEMBERSHIP IN ONE OF THE GENERATED VOCABULARIES — ONE LOOKUP FOR ALL THREE, because three vocabularies
   asking one question through three copies of a binary search is three right answers to one question, which
   is the shape that drifts. Each generated table is SORTED by its generator, so membership is a binary
   search — a linear scan would run on every unresolved global read, of which a forced-exec run does a great
   many.
   AND THE SORT IS NOW ACTUALLY ASSERTED, WHICH THE COMMENT THAT STOOD HERE CLAIMED AND THE CODE DID NOT DO.
   That claim was the dangerous kind: a check announced where the grep answers empty closes the question, so
   nobody looks. It is load-bearing rather than tidy, and the direction it fails in is the silent one — a
   binary search over a disordered table does not report an error, it MISSES, and a miss here answers a name
   the standard owns as server-injected app state, which is exactly the frontier multiplication this file's
   header records for the 22-name list, arriving through a generator instead of through a typed list.
   ONCE PER TABLE AND DEV-ONLY: the scan is O(n) and the answer cannot change within a process, so `checked`
   is the whole cost after the first call. `which` is the ADDRESS the abort would otherwise lack — this helper
   is reached over two vocabularies and stamps its own line for both, so the vocabulary travels with the
   operation rather than being derived here (CLAUDE.md §AN-ASSERT-THAT-NAMES-A-REMEDY).
   IT HANDS BACK THE TABLE'S OWN ENTRY RATHER THAN A YES, and that is not a convenience: the entry is a static
   string this file generated its tables from, so a caller may key on the POINTER and a page's bytes can never
   enter anything keyed that way. `strcmp(name, tbl[mid]) == 0` makes the two byte-identical, so returning the
   caller's `name` would be the same STRING and a different lifetime and a different provenance — and the
   census keyed on this answer would then hold a pointer into a JS_AtomToCString the hook frees on its way
   out. Returns NULL for "no table holds it", which no entry can collide with. */
static const char *names_find(const char *const *tbl, int n, const char *name, const char *which, int *checked)
{
    int lo = 0, hi = n - 1;

    DCHECK(name != NULL, "a generated vocabulary was asked about no name at all");
#if APICLIENT_DEV
    if (!*checked) {
        int i;

        *checked = 1;
        for (i = 1; i < n; i++)
            DCHECKF(strcmp(tbl[i - 1], tbl[i]) < 0,
                    "the %s vocabulary is not in strcmp order at entry %d (\"%s\" then \"%s\") — this "
                    "lookup is a binary search over a table its generator sorts, and a disordered table does "
                    "not report an error, it MISSES: a name the standard owns is then answered as "
                    "server-injected app state and a branch on it FORKS instead of naming the component this "
                    "engine owes",
                    which, i, tbl[i - 1], tbl[i]);
    }
#else
    (void)which;
    (void)checked;
#endif
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(name, tbl[mid]);
        if (c == 0)
            return tbl[mid];
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* One flag per table; the host is one agent per instance and nothing here is entered from a second thread. */
static int g_platform_sorted, g_language_sorted, g_i18n_sorted;

#if APICLIENT_DEV
/* THE THREE VOCABULARIES ARE DISJOINT, AND UNTIL THIS RAN THAT WAS TRUE ONLY BY CONVENTION. The lookup below
   returns the FIRST table that holds a name, so where two tables held one, search ORDER would silently decide
   which standard a `@WHY` names as owing it — and the order is written for cost (smallest last), which is no
   basis for answering that. Three generators fed by three independent artifacts cannot promise each other
   anything, so the promise is checked here instead of assumed: measured at 1285 / 58 / 1 entries with all
   three pairwise intersections EMPTY, and this is what keeps that true rather than merely observed.
   It runs ONCE, off the same latch the sort checks use, and the searches are hoisted out of the condition
   because a DCHECK's condition must be side-effect-free and names_find writes its `checked` latch. */
static void vocab_assert_disjoint(void)
{
    int i;

    for (i = 0; i < LANGUAGE_NAMES_N; i++) {
        const char *dup = names_find(PLATFORM_NAMES, PLATFORM_NAMES_N, LANGUAGE_NAMES[i],
                                     "Web IDL [Exposed=Window]", &g_platform_sorted);
        DCHECKF(dup == NULL, "\"%s\" is in BOTH browser/platform_names.h and browser/language_names.h, so "
                             "which standard owns it is decided by the order absent_standard_name happens to "
                             "search — and that order is written for cost. Two standards claiming one global "
                             "name is a fact about the standards, and this file may not resolve it by luck",
                LANGUAGE_NAMES[i]);
    }
    for (i = 0; i < I18N_NAMES_N; i++) {
        const char *dp = names_find(PLATFORM_NAMES, PLATFORM_NAMES_N, I18N_NAMES[i],
                                    "Web IDL [Exposed=Window]", &g_platform_sorted);
        const char *dl = names_find(LANGUAGE_NAMES, LANGUAGE_NAMES_N, I18N_NAMES[i],
                                    "ECMAScript §19 global-object", &g_language_sorted);
        DCHECKF(dp == NULL, "\"%s\" is in BOTH browser/i18n_names.h and browser/platform_names.h — see the "
                            "disjointness note above", I18N_NAMES[i]);
        DCHECKF(dl == NULL, "\"%s\" is in BOTH browser/i18n_names.h and browser/language_names.h — ECMA-402 "
                            "putting a name ECMAScript §19 already names would mean this engine owes it under "
                            "two standards, which is a fact to read rather than to search past", I18N_NAMES[i]);
    }
}
#endif

/* THE `Intl` RESIDUAL THAT STOOD HERE IS RETIRED BY THE TABLE ABOVE, WHICH IS THE CONDITION IT NAMED. What
   it argued is now CODE — browser/i18n_names.h and engine/i18nglobalgen.mjs — so the reasoning lives where it
   is executed rather than where it was deferred, and the one part of it a reader could still re-derive
   wrongly is kept AT the generator: the global name is the RECEIVER the members share, taken from the text
   BEFORE the dot, and NEVER by widening an IdentifierName test to admit the dot, which would enter
   `Intl.Collator` in a table of GLOBAL names.
   WHAT IS STILL OWED IS THE REGISTRATION AND NOT THE NAME, and the two were welded together until the load
   path was read: citegen's `audit()` iterates `for (const s of SPECS)` and loads indexes BY KEY, and nothing
   enumerates engine/specindex — so the §8-against-ECMAScript-§8 collision, and the whole-corpus measurement
   it forces, are the price of giving ECMA-402 a SPECS row rather than of claiming its name. The row buys the
   only regen path there is (`regen()` resolves through SPEC_BY_KEY and throws for an unknown key), which is
   why i18nglobalgen fetches where esglobalgen reads a committed index and why that generator carries its own
   retirement condition. This engine still builds NO part of ECMA-402; what changed is that its one global
   name is now answered by a standard instead of defaulting to server-injected app state. */
const char *absent_standard_name(const char *name, AbsentVocab *vocab)
{
    const char *e;

    DCHECK(vocab != NULL, "a global name was asked which standard owns it with nowhere to put the answer — "
                          "the vocabulary is the half of this answer a `@WHY` has to be able to state, and a "
                          "caller that wants only the yes/no is a caller that will re-derive it wrongly");
#if APICLIENT_DEV
    {
        static int checked;
        if (!checked) { checked = 1; vocab_assert_disjoint(); }
    }
#endif
    e = names_find(PLATFORM_NAMES, PLATFORM_NAMES_N, name, "Web IDL [Exposed=Window]", &g_platform_sorted);
    if (e) {
        *vocab = ABSENT_VOCAB_WEBIDL;
        return e;
    }
    e = names_find(LANGUAGE_NAMES, LANGUAGE_NAMES_N, name, "ECMAScript §19 global-object", &g_language_sorted);
    if (e) {
        *vocab = ABSENT_VOCAB_ECMASCRIPT;
        return e;
    }
    /* THE THIRD STANDARD, AND IT IS ASKED LAST ONLY BECAUSE IT IS SMALLEST — the three tables are DISJOINT, so
       the order is a cost and never a decision. A name in two of them would be a fact about the standards that
       this file must not resolve by search order, which is what the disjointness assert at the seal is for. */
    e = names_find(I18N_NAMES, I18N_NAMES_N, name, "ECMA-402 §8 The Intl Object", &g_i18n_sorted);
    if (e) {
        *vocab = ABSENT_VOCAB_ECMA402;
        return e;
    }
    return NULL;
}

/* ---- WHAT THIS DOCUMENT ASKED FOR AND THIS ENGINE DID NOT ANSWER ------------------------------------------
 *
 * THE ARM ABOVE IS CORRECT AND IT IS SILENT, AND THOSE ARE TWO DIFFERENT PROPERTIES. This file's header says
 * of an unbuilt Web API that "its ReferenceError is the forcing function that names the component to write",
 * and that is true of `new EventSource(url)` — the page throws, the throw carries the name, and
 * result.c's `pageErrors` carries the throw. It is FALSE of `if (window.EventSource)`, which is what a real
 * bundle writes instead: the read misses, the suppression below leaves it alone, §10.1.8.1 OrdinaryGet
 * ( O, P, Receiver ) step 2.b answers `undefined`, the guard takes its false arm, and the whole feature
 * branch behind it is unreachable with NOTHING ANYWHERE SAYING SO. What is lost is not the line — it is every
 * endpoint and every sink behind the guard, which is precisely the surface §What-the-tool-produces exists to
 * find. A run like that completes, emits, and looks healthy.
 *
 * SO THE SUPPRESSION RECORDS WHAT IT SUPPRESSED. Not a behaviour change — the arm decides exactly what it
 * decided before, and forcing the true arm of a guard over an API this engine cannot execute would manufacture
 * a flow that dies at the very next member call. A MEASUREMENT.
 *
 * THE POPULATION IS THE INTERSECTION AND THAT IS THE WHOLE POINT. It is not the 1333 names Web IDL exposes on
 * Window, and it is not the several hundred of them this build does not install: both of those are facts about
 * the BUILD, true of every run, and they say nothing whatever about a document. It is the names THIS DOCUMENT
 * READ that a standard owns and this realm did not answer — a document that never mentions `OffscreenCanvas`
 * is not missing coverage, and one that feature-detects it and takes the false arm is. That set is exactly
 * what reaching this arm means, so it is recorded HERE and nowhere else: the read MISSED (so nothing in this
 * realm answers the name) and a vocabulary CLAIMED it (so a standard owes an answer).
 *
 * KIND: every number below is a LIFETIME COUNT OF EVENTS — monotone within one agent's life, differenceable
 * across two samples of one document, and zeroed with the published-namespace registry at absent_free, which
 * concolic.c calls on the agent's release column. It shares that lifetime with `_candidates` and `_sourceReads`
 * on the same result document and NOT with `_switches`, which is the instance's own.
 *
 * THE DENOMINATOR IS EMITTED BESIDE THE ROWS, because a count of unanswered names states nothing on its own:
 * seven is a different fact out of ten global misses than out of ten thousand. The reads this file was told
 * about with the GLOBAL as base partition into exactly three, and all three are emitted:
 *   owed        — a standard owns the name and this realm has none: the rows below, summed.
 *   index       — an integer key on the global, refused by the arm that cites HTML §7.2.2.2 Indexed access on
 *                 the Window object.
 *   app state   — everything else: no vocabulary claimed the name.
 * `owed + index + appState == globalReads` is asserted in the composer, where all four are in one hand.
 * THE THIRD ARM IS NAMED FOR THE DECISION AND NOT FOR THE MINT, which it used to be and which stopped being
 * true the day a second entry reached this partition — and there are now THREE, which is the same argument
 * holding rather than a new one: a `typeof` read and an `in` read are each classified by the same three arms
 * and each mints nothing, so a counter called "mints" would have been a verb its own accessor no longer
 * performs (CLAUDE.md §READ-THE-ACCESSOR). What a reader who wants the mints does instead is subtract BOTH,
 * which is what the two operator members are for and why each is a SEPARATE CUT of the same denominator
 * rather than a fourth and fifth arm:
 *   typeof      — of those reads, the ones the typeof operator answered, which performed no [[Get]] and left
 *                 nothing behind. `typeof <= globalReads` is asserted beside the identity.
 *   in          — of those reads, the ones the `in` operator answered, likewise. `in <= globalReads` is
 *                 asserted beside it, as its OWN containment rather than as a sum with the one above.
 * A fourth ARM would have broken the partition; a CUT states a fact the partition cannot. They are two
 * members and not one "answered without a [[Get]]" total because a bundle writes ONE of the two spellings,
 * so the split is the whole content: which of them a document used is a fact about what that document's
 * false arms cost, and a sum of them is a number about nothing anybody asked.
 *
 * WHAT THIS IS NOT A FRACTION OF, STATED SO NOBODY READS IT AS ONE: it is not the fraction of the platform
 * surface this document uses. The hook is reached ONLY on a miss, so a name this engine DOES answer never
 * arrives here at all and the answered half of the document's ask is invisible from this file. A coverage
 * figure over the document's ask would need a count of global reads that HIT, which is a hook on the fast path
 * of every property read in the engine and is not this instrument.
 *
 * AND TWO OPERATORS ANSWER WITHOUT READING AND BOTH ARE RECORDED, WHICH IS WHY THE DENOMINATOR IS NOT THIS
 * HOOK'S ASK. A read spelled `typeof EventSource` compiles to OP_get_var_undef — the parser patches the
 * identifier's own OP_scope_get_var into it at `una_typeof_done`, which is that opcode's only producer — and
 * the engine answers ECMAScript §13.5.3 The typeof Operator's §13.5.3.1 Runtime Semantics: Evaluation step
 * 2.a, "If IsUnresolvableReference(value) is true, return "undefined".", at the opcode. Step 2.a settles it
 * BEFORE step 2.b's GetValue, so no [[Get]] is performed and this hook is not asked; minting there would make
 * step 2.b run and the operator say "object" where every browser says "undefined". A read spelled
 * `"EventSource" in window` compiles to OP_in, whose §13.10.1 Runtime Semantics: Evaluation ends at "Return ?
 * HasProperty(rightValue, ? ToPropertyKey(leftValue))" — §7.3.11 HasProperty ( obj, propertyKey ), which is
 * not §7.3.2 Get — so no [[Get]] is performed there either and a value handed back would answer a question
 * the standard does not ask. So the engine RECORDS on its way past each arm
 * (JSConcolicHooks.absent_unresolved -> absent_unresolved_note below) and decides nothing, and a name read
 * all three ways raises ONE row. The population these numbers are a fraction of is therefore "reads of a
 * global name this file was told about", by ANY of the three entries, and the two `_of those, answered by
 * the … operator` members split them — because the mint happens on only one of the three, so a reader who
 * wants the count of unknowns minted on the global subtracts both and cannot get it from the app-state arm
 * alone.
 *
 * THE `in` SPELLING IS BUILT AND THE RESIDUAL THAT DEFERRED IT IS RETIRED — AND WHAT IS KEPT IS THE PART OF
 * ITS NEXT-DIFF CLAUSE THAT WAS WRONG, because that clause was landed as an instruction and a later reader
 * would otherwise have built it. Two halves of it were exactly right and are what this diff is made of: a
 * field on JSOpKeyed naming the OPERATOR, which is a struct copied field-by-field by js_op_keyed_clone and so
 * carries an obligation there; and the reason a recording site at the [[HasProperty]] MISS is unbuildable,
 * since `gp_op = GP_HAS` on `ctx->global_obj` is the same request every identifier resolution issues and a
 * site there would count every unresolved identifier a second time.
 * WHAT WAS WRONG WAS THE SENTENCE THAT FOLLOWED FROM THAT AND THE WORK IT PRESCRIBED. It said the operator's
 * own placement "cannot host it either … and by then the base operand is gone", and therefore that the next
 * diff needed "the base carried to the placement". BOTH are false, and the refutation is in `do_opkeyed_place`
 * itself: its opening paragraph states that "the request's own arguments never touch the caller's stack, so
 * the operator's operands are still exactly where it left them and the answer replaces them", and the line
 * that frees them reads them off `sp` at that point. The base was never gone; it is `sp[-1]`, live until the
 * pop two lines later. The double-counting argument was TRUE and was aimed at the wrong site — identifier
 * resolution is `CONT_WITH_HAS` and a bytecode operator is `CONT_OP_KEYED`, so the placement is somewhere
 * identifier resolution cannot reach and the two populations were already separated by the request's own
 * outer kind. The clause also named `absent_unresolved_note` as the destination to reach, which would have
 * transplanted THIS file's key ASSERT onto a key the page computes: `in` takes `? ToPropertyKey(leftValue)`,
 * so `Symbol.iterator in window` is a page-held abort switch, and the engine filters that arm instead.
 * THE METHOD IS THE FINDING AND THE CLAUSE IS ITS SYMPTOM (CLAUDE.md §THE-MEASURED-RATE): every wrong half
 * was a claim about THIS TREE written by someone who knew exactly what was missing and was reasoning about
 * where the code SITS rather than reading what it DOES — thirty lines apart, in the function the clause named.
 * The spec half of the same residual was correct throughout.
 *
 * NAMED RESIDUAL — NOT COVERED: a [[HasProperty]] on the global reached through a CALL rather than through an
 * OPERATOR. `Reflect.has(window, "X")` and `Object.prototype.hasOwnProperty.call(window, "X")` ask the same
 * question about the same name and answer it with no [[Get]], exactly as `in` does — Reflect's is literally
 * the same `GP_HAS` request, declared at `js_reflect_has_def` — but they are METHODS, so their answer is
 * delivered into a step machine rather than onto the operand stack and they reach no operator placement. The
 * key rule is the same one: a call may hand either of them a symbol, so that arm would filter as this one
 * does. (`hasOwnProperty` is the narrower question — §7.3.12 HasOwnProperty, not §7.3.11 — and the global's
 * own chain makes the two answers differ for a name inherited from Object.prototype, which is a fact about
 * which of them a page may be asking and not a reason to record only one.)
 * WHAT THE NEXT DIFF BUILDS: the same three gates this arm carries — base is the global object, answer is
 * false, key is nameable — at the point `js_reflect_prop_step`'s GP_HAS answer is delivered, reaching
 * absent_unresolved_note with a third member of JSConcolicAbsentOp. The enum's switch here has no `default`,
 * so adding that member is a compile diagnostic at this function rather than a cut that silently stays zero.
 * HOW ITS ABSENCE SHOWS: for one document and one absent name, the census moves when the detection is spelled
 * with an operator and does not move when it is spelled with a call — so a reader comparing two documents'
 * owed counts is comparing how each bundle's authors happened to write a feature test, and the call spelling
 * is the one that reads as a document that asked for nothing. It is NOT shown by the corpus being empty of
 * the shape today: that is one run of an instrument over one set of mirrors, and it is the population this
 * census exists to measure at RUNTIME rather than to predict from a grep.
 *
 * THE DERIVATION FOR HOW MUCH EACH SPELLING IS WORTH IS A COMMAND AND NEVER A NUMBER HERE, and each of these
 * is a FLOOR: it counts one spelling of one shape over the mirrors this tree happens to hold, so a minified
 * alias for the receiver, a computed key and a quote style it does not list are all invisible to it. The
 * instrument that answers without a spelling in it is this census itself.
 *   `grep -rohE '"undefined"!=typeof [A-Za-z_$][A-Za-z0-9_$]*' --include='*.js' testing/corpus | wc -l`
 *   `grep -rohE '["'"'"'][A-Za-z_$][A-Za-z0-9_$]*["'"'"'] *in *(window|self|globalThis)\b' --include='*.js' testing/corpus | wc -l`
 *
 * NAMED RESIDUAL — NOT COVERED: whether a recorded read was the SILENT one. `js_absent_ask` has exactly two
 * callers — derive them rather than taking a line number, which has already gone stale here once:
 *   git grep -n 'js_absent_ask(ctx' -- engine/qjs/quickjs.c
 * (this prescribed `--recurse-submodules` and said the plain form answers that the interpreter has no
 * such call. That was true while engine/qjs was a GITLINK and is false now that it is ordinary tracked
 * content, where both forms answer alike — so the flag is owed to a PATH and never to this one, and the
 * way to know which is to ask rather than to read it here: `git cat-file -t origin/main:<path>` answers
 * `commit` for a gitlink and `tree` for content. What rotted was the REPOSITORY BOUNDARY and never the
 * two callers, which is the half a reader is tempted to doubt.) Both hand this file the same base and
 * the same atom: the property-read miss that degrades to
 * `undefined`, and the unresolvable Reference that goes on to throw. The `typeof` entry beside them is a
 * THIRD outcome and is separated by its own member AND by its own bucket on the row, which the other two are
 * not: they are the two callers of ONE entry and land together in `read`. The outcome differs entirely AFTER
 * this hook returns, so a row whose `read` bucket stands at twelve cannot be read as twelve silent
 * degradations — and the bucket is the unit of that ambiguity now rather than the row, since the `typeof` and
 * `in` buckets beside it are each one outcome and carry none of it.
 * WHAT THE NEXT DIFF BUILDS: the ask carrying which of its two callers asked, so the row splits into the arm
 * that degraded and the arm that threw — again an engine/qjs change.
 * HOW ITS ABSENCE SHOWS: a page that only ever writes `new EventSource(…)` is FULLY diagnosed by its own
 * ReferenceError in `pageErrors` and still appears here, so a reader cross-references the two surfaces by
 * hand; a name in this census with no matching `pageErrors` entry is the silent case and a name in both is
 * ambiguous. */
/* WHICH OF THIS FILE'S THREE ENTRIES RAISED A READ. IT IS A HOST FACT AND NOT THE ENGINE'S
   `JSConcolicAbsentOp`, and taking the engine's enum for it is the wrong diff a reader reaches for FIRST —
   recorded here because it is one member short of the population and the shortfall lands in the direction
   that inverts the finding. quickjs.h defines JSConcolicAbsentOp as "WHICH OPERATOR ASKED A GLOBAL NAME AND
   ANSWERED WITHOUT PERFORMING A [[Get]]" and says of BOTH its members that "neither operator's algorithm
   contains a read", so the [[Get]] absent_read_hook answers has no member of it and cannot be given one
   without contradicting the sentence that defines the enum. A field of that type on a row would therefore be
   STATED at one of global_miss_note's two callers and DEFAULTED at the other, and the defaulted one is the
   larger: every property read on the global would land in whichever member the default named. That is
   CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS at the one axis this census exists to publish — a `typeof` or an
   `in` guard degrades to the false arm a browser without the feature also takes, and a read nothing guarded
   ends the flow at a ReferenceError, so filing the second under the first reports the cheapest band for the
   most expensive one.
   SO THE ENGINE'S TWO MAP INTO THIS THREE AND NEVER THE OTHER WAY ROUND: the engine states which no-[[Get]]
   operator asked because it is the only party that knows, and this file states which of ITS entries was
   standing there because the engine does not know this file has three. `ABSENT_ENTRY_N` is the COUNT and is
   never an entry — it is the poison an unset local carries into owed_note's own assert, exactly as
   `ABSENT_VOCAB_N` is, so an arm that classifies nothing aborts at the record with the name in hand rather
   than filing one spelling under another's.
   RETIREMENT: this argument goes when JSConcolicAbsentOp can name a [[Get]], which would mean the engine's
   `.absent` and `.absent_unresolved` hooks had become one — at which point the two enums are one enum and
   nothing here is re-derivable. */
typedef enum { ABSENT_ENTRY_READ, ABSENT_ENTRY_TYPEOF, ABSENT_ENTRY_IN, ABSENT_ENTRY_N } AbsentEntry;

/* AND THE SPLIT IS AN ARRAY OF COUNTS RATHER THAN ONE MEMBER NAMING AN ENTRY, which is the second half of the
   same argument and is independent of it. A row is keyed by NAME and accumulates for the life of the run, so
   what it describes is N EVENTS and a scalar is one latch over all of them: `typeof X !== "undefined" && new
   X(...)` is the commonest shape a bundle has and it raises two entries on ONE row, which a latch answers
   with whichever ran last. Counts are also what make the split CHECKABLE rather than merely published — the
   buckets SUM to `reads`, and a total that cannot move without one of its parts moving is the only kind a
   reader may do arithmetic on (CLAUDE.md §A-GAUGE-AND-A-LIFETIME-COUNTER). A latch closes over nothing.
   RETIREMENT: this argument goes when a row can hold at most one read, which is what a seen-set would buy and
   is banned by §NO BOUNDS, so it does not go. */
typedef struct { const char *name; long reads; long by_entry[ABSENT_ENTRY_N]; AbsentVocab vocab; } OwedRow;
static OwedRow *g_owed;
static int g_owed_n, g_owed_cap;
/* THE THREE ARMS OF THE GLOBAL MISS, EACH RAISED AT THE SITE THAT DECIDES IT — never one counter incremented
   in three places, because the whole value of the partition is that a reader can see WHICH arm moved. */
static long g_owed_reads, g_index_refused, g_appstate_reads;
/* AND THE POPULATION THEY PARTITION, RAISED SEPARATELY AT THE TOP OF THAT ARM. It is not the sum of the three
   — see the raise for why deriving it would make the composer's identity a restatement instead of a check. */
static long g_global_reads;
/* AND TWO CUTS OF THAT SAME POPULATION, NEITHER OF WHICH IS AN ARM OR MAY BE ONE. The three arms answer
   "what was the name"; these answer "which operator asked", and the two questions are independent — a
   `typeof` or an `in` read lands in whichever of the three arms its NAME decides, exactly as a property read
   does. Kept as cuts rather than folded into the arms because the mint happens on exactly ONE of the three
   entries, so `app state − typeof − in` is the only expression from which a reader can recover how many
   unknowns were actually minted on the global. Containment is asserted per cut in the composer, where each
   and its denominator are in one hand (CLAUDE.md §a-count-offered-as-a-share).
   THEY ARE TWO MEMBERS AND NOT ONE `answered without a [[Get]]` TOTAL, and that is the whole reason the
   engine's hook carries an operator at all. A reader who finds a name in this census and wants to know why
   the page did not throw has to know which operator asked: `typeof X` and `"X" in window` take DIFFERENT
   false arms in a bundle and a page that writes one does not write the other, so a single total would be a
   number about a question nobody asked. It is also what keeps the cuts CHECKABLE — two counters raised at two
   entries, each with its own containment, part company independently. */
static long g_unresolved_reads;
/* THE SECOND OF THEM — `"X" in window`, ECMAScript §13.10.1 Runtime Semantics: Evaluation's
   `RelationalExpression : RelationalExpression in ShiftExpression`. It is a cut for g_unresolved_reads'
   reason exactly and it is a SEPARATE cut for the reason stated there. */
static long g_in_reads;
/* AND THE SAME OWED READS SPLIT BY WHICH STANDARD OWES THEM, which is the fact absent_standard_name exists to
   carry: an unbuilt Web IDL interface is a browser component to write and an uninstalled ECMAScript §19 name
   is a language intrinsic this build did not link, and those are two different pieces of work. */
static long g_owed_by_vocab[ABSENT_VOCAB_N];

/* RECORD ONE READ OF A NAME A STANDARD OWNS AND THIS REALM DOES NOT ANSWER. `name` is the TABLE'S entry, so
   the row is keyed by POINTER: two reads of one name are one row, and a page cannot get its bytes in here.
   NO CAP AND NO SEEN-SET (CLAUDE.md §NO BOUNDS). The table grows for as long as a document reads new names,
   and a SECOND read of a name already in it raises that row rather than being dropped — the two facts a
   reader needs are how MANY names went unanswered and how HARD the document leant on each, and a set would
   answer only the first while looking like it answered both.
   THE SCAN IS LINEAR AND IS POINTER EQUALITY, which is cheap for the reason the population is: this runs only
   where a read has ALREADY missed the whole prototype chain of the global AND matched a binary search, so the
   distinct names are what one document asked for and not what a standard defines. A hash here would be a
   second index over a table whose whole content is normally under a hundred rows. */
static void owed_note(const char *name, AbsentVocab vocab, AbsentEntry entry)
{
    int i, e;

    DCHECK(name != NULL, "a read was recorded as owed with no name — the caller's name is the vocabulary's own "
                         "entry and absent_standard_name returns NULL rather than an empty one, so this is a "
                         "caller recording the MISS arm of that lookup as though it were the hit arm");
    /* A `CHECK` AND NOT A `DCHECK`, BECAUSE THE NEXT LINE INDEXES AN ARRAY WITH IT. The value is this
       codebase's own — absent_standard_name writes it on every hit and the enum is the only thing that mints
       one — so asserting on it is legitimate; what decides the MACRO is that the guarded operation happens in
       EVERY build. A dev-only guard over a release-mode indexed write is CLAUDE.md's promoted-DCHECK defect:
       the check is compiled out of exactly the build where the write still happens, trading a dev abort for a
       wild store into whatever follows the counters. */
    CHECKF(vocab < ABSENT_VOCAB_N, "a read was recorded as owed by a standard this file has no vocabulary for "
                                   "(%d of %d) — the enum is the only thing that mints one and "
                                   "absent_standard_name writes it on every hit, so an out-of-range value is a "
                                   "caller passing an uninitialised local, and the line below indexes the "
                                   "per-standard split with it", (int)vocab, (int)ABSENT_VOCAB_N);
    /* AND THE SAME MACRO FOR THE SAME REASON ONE ARGUMENT OVER — the row's bucket is indexed with this in
       every build, so a dev-only guard here would be compiled out of exactly the build where the write still
       happens. The value that reaches an out-of-range state is the POISON global_miss_note's two callers
       carry when nothing classified them, so this fires with the NAME in hand: `ABSENT_ENTRY_N` arriving
       means a caller reached the record without saying which of this file's entries it was, and the arm that
       does that is a switch over JSConcolicAbsentOp that gained a member. */
    CHECKF(entry < ABSENT_ENTRY_N, "a read was recorded as owed by an entry this file does not have (%d of "
                                   "%d) — the enum is the only thing that mints one and every caller states "
                                   "it, so an out-of-range value is the poison an unclassified arm carries "
                                   "and the line below indexes the row's per-entry split with it",
           (int)entry, (int)ABSENT_ENTRY_N);
    g_owed_reads++;
    g_owed_by_vocab[vocab]++;
    for (i = 0; i < g_owed_n; i++)
        if (g_owed[i].name == name) {
            g_owed[i].reads++;
            g_owed[i].by_entry[entry]++;
            return;
        }
    if (g_owed_n == g_owed_cap) {
        int cap = g_owed_cap ? g_owed_cap * 2 : 8;
        OwedRow *rows = (OwedRow *)realloc(g_owed, sizeof(*rows) * (size_t)cap);
        /* CHECK AND NOT A SILENT DROP, for this file's own reason one function up: the alternative to failing
           here is a census that reads CLEAN on a document that asked for a name nothing answered, which is the
           silence this whole surface exists to end, arriving through the allocator. */
        CHECK(rows != NULL, "absent: OOM growing the table of standard-owned names this realm did not answer — "
                            "the alternative to failing here is publishing a census that reads clean for a "
                            "document whose feature branches were all taken false");
        g_owed = rows;
        g_owed_cap = cap;
    }
    g_owed[g_owed_n].name = name;
    g_owed[g_owed_n].reads = 1;
    /* ZEROED BY HAND AND NOT BY THE ALLOCATOR, because the allocator above is `realloc` and realloc does not
       zero what it grows into. A new row's buckets would otherwise hold whatever the heap last put there, the
       composer's per-row identity would fire on the first census, and on the arm where DCHECKs are compiled
       out the census would publish a split of a population it never counted. */
    for (e = 0; e < ABSENT_ENTRY_N; e++)
        g_owed[g_owed_n].by_entry[e] = 0;
    g_owed[g_owed_n].by_entry[entry] = 1;
    g_owed[g_owed_n].vocab = vocab;
    g_owed_n++;
}

/* WHAT A MISS ON THE GLOBAL WAS, IN ONE FUNCTION, BECAUSE THREE ENTRIES ASK IT AND ONLY ONE OF THEM PERFORMS
   A [[Get]]. The read hook asks so it can decide what to answer; absent_unresolved_note asks so it can record
   a `typeof` or an `in` the engine has already answered. The classification is IDENTICAL for both — it is a question about
   the NAME and about nothing else — and a second copy of it would be two answers to one question, which is
   the shape this file's own header says drifts. So the arms are here, each still raised where it is decided,
   and the denominator is raised here too: derived as the sum of the arms it could not disagree with them
   under any state of this function, and an assert whose two sides cannot disagree is not a weak check but a
   NON-check that certifies whatever it never examined. Raised at the top, it fires the day an arm is added
   that leaves this function without classifying its read.
   IT COUNTS WHAT THIS FILE WAS TOLD ABOUT and not "every global miss": every entry gates on the key rule
   first, so a symbol never arrives and is in neither the numerator nor this. WHY it holds differs per entry
   and that difference is not cosmetic — js_absent_ask FILTERS, the typeof arm has the identifier grammar and
   ASSERTS, and the `in` arm FILTERS because §13.10.1's key is the page's own `? ToPropertyKey(leftValue)`.
   One rule, three reasons; a reader who takes the reason for the rule will put an assert on a page's bytes.
   Returns 1 when the read is LEFT ALONE — a standard owes the name, or it is an integer key on the global —
   and 0 when no vocabulary claimed it, which is the arm the read hook mints an unknown on and the two
   operator entries simply record. `key` is the atom already spelled by ns_key_str.
   `entry` IS WHICH OF THE THREE ASKED AND IS THE CALLER'S TO STATE, for the reason the engine's own operator
   enum is the engine's to state: this function cannot infer it, because the classification it performs is a
   question about the NAME and the name is identical at all three entries. It is carried rather than derived
   and it reaches the ROW, which is what separates "this document guarded the name" from "this document read
   it" per NAME instead of only in the two whole-run cuts above. */
static int global_miss_note(const char *key, JSAtom name, AbsentEntry entry)
{
    /* THE POISON IS THE VALUE owed_note'S OWN ASSERT NAMES, so a vocabulary that is read without having been
       written aborts at the record with the name in hand rather than filing one standard's work under the
       other's. `absent_standard_name` writes it on every hit and leaves it alone on the miss, and the miss is
       the arm where nothing reads it. */
    AbsentVocab vocab = ABSENT_VOCAB_N;
    const char *owed;

    /* THE CLASSIFICATION IS ASSERTED AT ITS ORIGIN AND NOT ONLY WHERE IT IS INDEXED WITH, which is two
       obligations rather than one copy of a third: owed_note guards an ARRAY WRITE and so is a CHECK in every
       build, and this guards that A CALLER CLASSIFIED ITS READ AT ALL, which is a statement about this
       codebase's own logic and so is a DCHECK. They do not cover the same population either — owed_note is
       reached only on the arm where a standard owns the name, so the poison an unclassified arm carries would
       pass through every app-state and every integer read in silence and fire only if the document happened
       to read an OWED name with the unclassified operator. Here it fires on the first read of any kind. */
    DCHECKF(entry < ABSENT_ENTRY_N,
            "a read of the global object reached the classification with no entry stated (%d of %d) — every "
            "caller of this function names which of the three it is, and the only value that is none of them "
            "is the poison absent_unresolved_note opens with, so this is a switch over JSConcolicAbsentOp "
            "that gained a member and classified it nowhere",
            (int)entry, (int)ABSENT_ENTRY_N);
    g_global_reads++;
    /* A name a STANDARD owns on the global object is a component this engine owes; leave the read alone
       so its throw names it. Asked ONLY of the global, because those names live there: `gon.Node` is a
       field of an app record that happens to be spelled like an interface, and suppressing it would
       answer a real unknown with `undefined`.
       THREE VOCABULARIES, BECAUSE THREE STANDARDS OWN NAMES HERE AND THIS ARM USED TO ASK ABOUT ONE. See this
       file's header for what asking about only Web IDL left falling through. */
    owed = absent_standard_name(key, &vocab);
    if (owed) {
        /* AND IT IS RECORDED ON THE WAY PAST, which is the whole of this file's answer to the silence the
           suppression creates. The decision is UNCHANGED — nothing forks, nothing is minted, the read is
           left exactly as alone as it was — and what the census gains is the one population that says
           something about a run: the names THIS DOCUMENT asked a standard for that this realm did not
           answer. See the census banner above owed_note for the population and the denominator. */
        owed_note(owed, vocab, entry);
        return 1;
    }
    /* AND AN INDEX IS NOT A FIELD OF A RECORD AT ALL, WHICH IS A QUESTION ABOUT THE KEY SPACE AND NOT
       ABOUT WHICH VOCABULARY OWNS A NAME. The channel's key rule admits array indices because a server's
       state tree is records inside LISTS and the walk reaches an element by its index —
       `__STATE__.users[0]` — and THE GLOBAL IS NOT A LIST. An integer key on it is HTML §7.2.2.2 Indexed
       access on the Window object, which says of it that "Indexed access to document-tree child navigables
       is defined through the [[GetOwnProperty]] internal method of the WindowProxy object": the
       extent is the INTERFACE'S, exactly as Web IDL §3.8 Platform objects implementing interfaces makes a
       platform object's member list the interface's rather than the document's. THIS ENGINE OWNS THE
       NAVIGABLE TREE, so an index past the child-navigable count has a real answer that this run computed
       — §10.1.8.1 OrdinaryGet ( O, P, Receiver ) step 2.b's `undefined` — and minting an unknown for it is
       the "record wrongly ON the channel" direction this file's header calls the silent one: nothing
       throws, and a `for (i = 0; i < window.length; i++) window[i]` walk performs it once per iteration
       past the end.
       IT IS THIS ARM'S RULE AND NOT ns_join'S. A document that writes `window[0] = {…}` in an inline
       script publishes a record there in its own right, and that record's members are read through the
       RECORD arm of the read hook — which is why the composer still spells an index off the root and why
       refusing one HERE takes nothing away: a server that injected at an integer key made it PRESENT, and a
       present key never reaches either entry.
       THE `typeof` ENTRY CANNOT REACH THIS ARM and it is not carved out of it, which is the difference
       between a fact and a special case: `typeof` takes an IDENTIFIER, ECMAScript §12.7 Names and Keywords
       gives no IdentifierStart that is a decimal digit, and an atom is a tagged integer only for the
       canonical numeric string §6.1.7 The Object Type defines — so the arm is unreachable from there by the
       grammar rather than by a test. Writing that test at the caller would be a second spelling of a rule
       this function already owns. */
    if (JS_AtomIsIndexName(name)) {
        g_index_refused++;
        return 1;
    }
    /* THE THIRD ARM, COUNTED WHERE IT IS DECIDED AND NOT WHERE IT IS PERFORMED — which is what lets a second
       entry share it at all. The mint is the read hook's and is shared with the record arm, so raising it
       there would count a published record's absent member among the global's — two populations under one
       name, which is the defect this partition exists to end one level up — and it would also count nothing
       for a `typeof`, which decides this same arm and mints nothing. Reaching this line IS the global arm
       deciding that no vocabulary claimed the name, which is the decision the row reports. */
    g_appstate_reads++;
    return 0;
}

/* ONE APPEND, TWO PASSES, AND NO HAND-COUNTED SIZE — solver/compose.h's rule applied to a LOOP, which is the
   one shape `composef` cannot serve because the row count is a fact about the document. `out == NULL` is the
   MEASURING pass: C99 §7.19.6.5 "The snprintf function" — "If n is zero, nothing is written, and s may be a
   null pointer" — so the same argument list is measured and then written with nothing counted by hand between,
   and there is no margin for a miscount to hide in (compose.h states what that margin cost twice).
   THE FIT IS ASSERTED ON THE WRITING PASS ONLY, because on the measuring pass there is no buffer to overrun
   and `cap - len` would be an underflow of the very quantity being computed. */
static size_t absent_emitf(char *out, size_t cap, size_t len, const char *fmt, ...) APICLIENT_PRINTF(4, 5);
static size_t absent_emitf(char *out, size_t cap, size_t len, const char *fmt, ...)
{
    va_list ap;
    int need;

    va_start(ap, fmt);
    need = out ? vsnprintf(out + len, cap - len, fmt, ap) : vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    CHECK(need >= 0, "absent: the owed-name census could not be MEASURED — vsnprintf reported an encoding "
                     "error, so there is no length to allocate against and any size chosen here would be the "
                     "hand-counted guess solver/compose.h exists to stop anyone writing");
    DCHECK(!out || (size_t)need < cap - len,
           "the owed-name census wrote past the length its own measuring pass computed — the two passes read "
           "one argument list over one table, so they can only disagree if a row changed between them, and "
           "what a truncation here loses is not a digit but the CLOSING BRACE: the document that embeds this "
           "census would not parse and every finding for the page would be discarded");
    return len + (size_t)need;
}

/* THE CENSUS SERIALIZES ITSELF, which is the rule solver/result.c states for every surface it composes and
   which solver/decide.c's `_forkAt` follows for the same reason: the keys are this file's own vocabulary and
   the escaping question is a fact about this table's contents, so it belongs where the table is.
   NO ESCAPING PASS, AND THAT IS ASSERTED RATHER THAN BELIEVED. A row's key is a GENERATED VOCABULARY'S OWN
   ENTRY — absent_standard_name returns `tbl[mid]` and owed_note keys on that pointer — so it is this
   codebase's bytes and not the page's, which is the one thing that makes writing it raw sound. decide.c's
   rows are the PAGE's bytes and escape every one of them; the difference is provenance and nothing else, so
   the provenance is what is checked below.
   MEMBERS OPEN ON `_` AND ROWS CANNOT, which is decide.c's namespace rule and is asserted here for its
   reason: a consumer sums the ROWS to get the reads and reads the MEMBERS as the population they are drawn
   from, so a member that could be mistaken for a row would put the denominator inside its own numerator. A
   ROW IS A HISTOGRAM AND A MEMBER IS A NUMBER, and summing a row now means summing its BUCKETS — which is
   the same statement one level down and is why the buckets are a partition rather than a list: they sum to
   that row's reads, so a reader who sums every bucket of every row still has the numerator.
   No name either standard puts on the global object opens on an underscore, and that is a claim about two
   generated tables, so it is a DCHECK over this codebase's own bytes and not over a document's.
   IT IS NEVER `{}`. Every member is emitted on every census, zeroes included, so this object has rows only
   when a document actually asked for something this realm could not answer and has MEMBERS always — which is
   what makes the clean day readable: `owed 0 of 4128 global reads` is a positive statement that this engine
   answered everything the standards were asked for, and it is the line that would otherwise appear only when
   something was wrong and so be a line nobody had learned to look for. Caller frees. */
char *absent_json(void)
{
    /* THE MEMBER NAMES, READ AS PROSE WITH THEIR VALUE APPENDED, because extension/popup.js renders a census
       generically as `key value` — so a terse field name arrives at a person in the exact shape of a row, and
       a row here is the name of a component this engine owes. decide.c's bound member is named the same way
       and for the same reason. */
    static const char KEY_READS[] = "_reads of the global object this file was asked about";
    static const char KEY_OWED[]  = "_of those, reads of a name below — a standard owns it and this realm has none";
    static const char KEY_INDEX[] = "_of those, refused as an integer key on the global";
    static const char KEY_APP[]   = "_of those, no standard owns the name — server-injected app state";
    /* THE FIRST OF TWO CUTS OF THE SAME DENOMINATOR, NEITHER OF WHICH IS ONE OF THE ARMS ABOVE — see
       g_unresolved_reads. Together they are what a reader subtracts to get the unknowns actually MINTED on
       the global, which the app-state arm alone stopped being able to say the day a second entry reached
       this partition and has been less able to say with every entry since. */
    static const char KEY_TYPEOF[] = "_of those, answered by the typeof operator with no [[Get]] performed";
    /* AND THE OTHER OPERATOR THAT ANSWERS WITHOUT READING, WHICH IS A THIRD MEMBER AND NOT A SECOND TOTAL.
       `"X" in window` is ECMAScript §13.10.1's HasProperty and never a [[Get]], exactly as `typeof X` is
       §13.5.3.1 step 2.a — but a bundle writes ONE of the two and the false arms it takes are different code,
       so a member that added them would answer a question nobody asks.
       THE NAME USED TO BE SPELLED SO THAT NO READER OF THIS CENSUS COULD NEWLY MATCH IT, and that constraint
       is RETIRED rather than merely satisfied — recorded because a reader who re-derives it will re-impose it
       on the next member for nothing. testing/corpus/site.mjs picked two members by distinctive SUBSTRINGS of
       their prose and asserted each matched exactly one key, so a member sharing either turned that reading
       into a hard error (CLAUDE.md §AND-THE-MIRROR-OF-THAT-IS-A-NEW-KEY). It no longer matches prose at all:
       testing/absent_census.js parses the declarations below and resolves a member by its C IDENTIFIER, which
       is the stable token, and compares the member SET it derives against the one a census actually carries.
       So a new member here may say anything, and what this file owes its readers is instead the rule two
       paragraphs down — a member OPENS ON `_` and a row cannot, which is what tells the two apart with no
       list of names at either end. */
    static const char KEY_IN[]    = "_of those, answered by the in operator with no [[Get]] performed";
    /* ONE PER VOCABULARY AND INDEXED BY THE ENUM, which is what the header's `ABSENT_VOCAB_N` promise buys: a
       third standard adds a row to this array and to `g_owed_by_vocab`, and nothing else here changes. */
    static const char *const KEY_VOCAB[ABSENT_VOCAB_N] = {
        "_of those owed reads, names Web IDL exposes on Window",
        "_of those owed reads, names ECMAScript §19 The Global Object puts there",
        "_of those owed reads, names ECMA-402 §8 The Intl Object puts there"
    };
    char *out = NULL;
    /* THE BUCKET NAMES, SHORT WHERE THE MEMBERS ABOVE ARE PROSE — a member is rendered by popup.js as a row
       of its own and is read as a sentence, and a bucket is rendered inside one (`EventSource read 1, typeof
       1, in 1`), so the vocabulary the members establish is what these three lean on rather than restating.
       THE SIZE IS DERIVED AND THE COUNT IS ASSERTED AT COMPILE TIME, which is the one thing the per-standard
       array beside it cannot do: `KEY_VOCAB[ABSENT_VOCAB_N]` is a SIZED array with a short initialiser, so a
       vocabulary added without a key compiles and writes `(null)` into the census. An unsized array plus this
       assertion makes that state unreachable rather than merely asserted — CLAUDE.md §Fix-the-ROOT — and it
       costs one line, so the sized form below is the one to change next rather than the pattern to copy. */
    static const char *const KEY_ENTRY[] = { "read", "typeof", "in" };
    size_t cap = 0, len = 0;
    long sum = 0, rowsby[ABSENT_VOCAB_N], rowsbyentry[ABSENT_ENTRY_N];
    int pass, i, v, e;

    _Static_assert(sizeof KEY_ENTRY / sizeof *KEY_ENTRY == ABSENT_ENTRY_N,
                   "absent.c's census has an entry with no bucket name, or a name with no entry");
    for (v = 0; v < ABSENT_VOCAB_N; v++)
        rowsby[v] = 0;
    for (e = 0; e < ABSENT_ENTRY_N; e++)
        rowsbyentry[e] = 0;
    /* THE ROWS SUMMED TWICE OVER, ONCE WHOLE AND ONCE PER STANDARD — which is what makes the per-standard
       split CHECKABLE rather than merely published, and what gives `OwedRow.vocab` a reader. That field is
       written at every row and it would otherwise be read by nothing at all: a name WRITTEN somewhere and
       READ nowhere is the broken contract this whole census exists to make visible one level up, and landing
       one inside it would be the instrument committing the defect it reports. What it must NOT be is the
       assert a reader reaches for first — "the row's vocabulary equals what absent_standard_name would say
       today" cannot fail, because that lookup searches the two tables in a fixed order and answers one
       vocabulary per name for the life of the process, and an assert whose two sides cannot disagree is a
       NON-check that certifies whatever it never examined. THESE two sides can: one accumulates per EVENT in
       owed_note and the other per ROW here, so they part company if the grow path drops a row or a bucket is
       raised twice. */
    for (i = 0; i < g_owed_n; i++) {
        long rsum = 0;

        sum += g_owed[i].reads;
        for (e = 0; e < ABSENT_ENTRY_N; e++) {
            rsum += g_owed[i].by_entry[e];
            rowsbyentry[e] += g_owed[i].by_entry[e];
        }
        /* THE ROW'S OWN IDENTITY, WHICH IS WHAT MAKES THE SPLIT A PARTITION RATHER THAN THREE ANNOTATIONS —
           and the two sides CAN disagree, which is the whole test an assert has to pass here. `reads` is
           raised by owed_note and so is the bucket, but the row is grown by `realloc`, which does not zero:
           a new row whose buckets were left as the heap found them parts company with its own total on the
           very first census. It also fires for a bucket raised twice for one read, which is how a second
           recording site gets added wrongly, and for a struct the two translation units lay out differently.
           A reader takes this row's split as a fraction OF `reads`, so a row that leaks hands back a
           fraction of a denominator that is not the number of reads. */
        DCHECKF(rsum == g_owed[i].reads,
                "the owed-name census holds a row (\"%s\") whose per-entry split does not sum to its own "
                "reads (%ld by the buckets, %ld by the row) — owed_note raises exactly one bucket per read "
                "and zeroes every bucket of a row it creates, so a mismatch is a row the grow path left "
                "uninitialised or a bucket raised beside a read rather than as part of one",
                g_owed[i].name, rsum, g_owed[i].reads);
        /* A `CHECK` FOR THE REASON THE RAISE HAS ONE — the line under it indexes `rowsby` with this value in
           every build, and a guard that is compiled out of the build where the write still happens is not a
           guard. It is NOT an `if` past a broken invariant either: there is no arm here that could be right,
           since a row filed under no vocabulary is a row this file cannot report at all. */
        CHECKF(g_owed[i].vocab < ABSENT_VOCAB_N,
               "the owed-name census holds a row (\"%s\") filed under a standard this file has no vocabulary "
               "for — owed_note asserts the same thing at the raise, so a row that reaches the composer with "
               "one is a row written past the end of the table or a struct laid out differently by the two",
               g_owed[i].name);
        rowsby[g_owed[i].vocab] += g_owed[i].reads;
    }
    /* THE THREE IDENTITIES, ASSERTED WHERE ALL THEIR TERMS ARE IN ONE HAND — CLAUDE.md §A-GAUGE-AND-A-LIFETIME
       -COUNTER: a quantity whose kind a reader cannot name from its output is one they are not entitled to do
       arithmetic on, and the identity is the one property of a counter a reader can actually check. */
    DCHECK(sum == g_owed_reads,
           "the owed-name census's rows do not sum to the owed reads it counted, and the object is where that "
           "identity is READ — a consumer takes the share of unanswered reads off these rows, so a partition "
           "that leaks hands back a fraction of a denominator that is not the number of reads. Every raise of "
           "the total is owed_note's and owed_note raises exactly one row with it, so a mismatch is a row "
           "dropped by the grow path or a total raised by a second writer");
    DCHECK(g_owed_reads + g_index_refused + g_appstate_reads == g_global_reads,
           "the three arms of the global miss do not sum to the reads this hook was asked about — the "
           "denominator is raised ONCE at the top of that arm and each arm raises its own counter, so this "
           "fires exactly when a fourth way out of the block was added without classifying its read. Every "
           "number in this census is a fraction of that denominator, so an unclassified arm does not make one "
           "row wrong, it makes the whole object a partition of a population it no longer covers");
    /* AND THE SECOND CUT IS CONTAINED IN THE POPULATION IT IS A CUT OF, which is the one property of a share
       a reader can check without re-deriving the mechanism (CLAUDE.md §a-count-offered-as-a-share). The two
       are raised at DIFFERENT events — the denominator once per classified read in global_miss_note, this one
       once per `typeof` entry before it classifies — so they part company exactly when an entry raises this
       without going through that function, which is the way a second recording site would be added wrongly. */
    DCHECKF(g_unresolved_reads <= g_global_reads,
            "the typeof cut of the owed-name census (%ld) is larger than the population it is a cut of (%ld) "
            "— every read this file is told about is classified by global_miss_note, which raises the "
            "denominator, so a larger cut is a caller that recorded a typeof read and never classified it",
            g_unresolved_reads, g_global_reads);
    /* AND THE SAME FOR THE `in` CUT, AS ITS OWN ASSERT RATHER THAN AS A SUM OF THE TWO. `typeof + in <=
       globalReads` is a WEAKER statement that closes over one cut running ahead while the other runs behind,
       and which of the two operators a document actually writes is the fact these members exist to state —
       so the pair is asserted per cut for the same reason the per-standard split below is asserted per
       standard rather than as one total. */
    DCHECKF(g_in_reads <= g_global_reads,
            "the `in` cut of the owed-name census (%ld) is larger than the population it is a cut of (%ld) — "
            "the engine records this arm from do_opkeyed_place and every read it records is classified by "
            "global_miss_note, which raises the denominator, so a larger cut is a recording that skipped the "
            "classification. The likeliest way to build one is a second call site for the same arm: there is "
            "exactly ONE, and it is the placement, because the [[HasProperty]] miss underneath it is the same "
            "request every unresolved identifier issues",
            g_in_reads, g_global_reads);
    /* AND THE SAME TWO CUTS AGAIN, THIS TIME AGAINST THE PER-NAME SPLIT THAT IS A CUT OF THEM — which is what
       gives `OwedRow.by_entry` a reader beside the row it is emitted on, exactly as the sum above the
       identities gives `OwedRow.vocab` one. The two sides are raised at DIFFERENT events: the cut once per
       operator entry BEFORE the classification, this sum once per OWED read inside it, so they part company
       when a row's operator bucket is raised without the entry that owns it having been reached — which is
       precisely the shape a second recording site for one of these operators would have.
       IT IS `<=` AND THE SLACK IS A POPULATION RATHER THAN A TOLERANCE, which is why it is stated here: the
       residue is operator reads on names NO standard owns, and `typeof __NEXT_DATA__` is the commonest line
       in a server-rendered bundle. A reader who takes the gap for a defect has read a cut of the global
       reads as a cut of the owed ones; an EQUALITY here would be that misreading frozen into an assert and
       would fire on the first app-state feature detect any document performs. */
    DCHECKF(rowsbyentry[ABSENT_ENTRY_TYPEOF] <= g_unresolved_reads,
            "the owed-name census's per-name typeof buckets (%ld) outnumber the typeof reads the engine told "
            "this file about (%ld) — the cut is raised once per typeof entry before the classification and "
            "the buckets once per OWED typeof read inside it, so the buckets can only be FEWER (the residue "
            "is typeof reads on names no standard owns). More of them is a row's bucket raised by something "
            "that never came through absent_unresolved_note",
            rowsbyentry[ABSENT_ENTRY_TYPEOF], g_unresolved_reads);
    DCHECKF(rowsbyentry[ABSENT_ENTRY_IN] <= g_in_reads,
            "the owed-name census's per-name `in` buckets (%ld) outnumber the `in` reads the engine told this "
            "file about (%ld) — stated as its own assert rather than as a sum with the typeof pair for that "
            "pair's own reason: a summed statement closes over one bucket running ahead while the other runs "
            "behind, and which operator a document actually writes is the fact this split exists to state",
            rowsbyentry[ABSENT_ENTRY_IN], g_in_reads);
    for (v = 0; v < ABSENT_VOCAB_N; v++)
        DCHECKF(rowsby[v] == g_owed_by_vocab[v],
                "the per-standard split of the owed reads disagrees with the rows it is a split OF (standard "
                "%d: %ld by the rows, %ld by the counter) — the counter is raised once per READ in owed_note "
                "and this sum is taken once per ROW here, so the two part company exactly when a row is "
                "dropped by the grow path or a bucket is raised twice. It is stated per standard rather than "
                "as one total because the total closes over a bucket raised for the WRONG standard while the "
                "split does not, and which standard owes the work is the whole content of this pair: an "
                "unbuilt Web IDL interface is a browser component to write and an uninstalled ECMAScript §19 "
                "name is a language intrinsic this build did not link",
                v, rowsby[v], g_owed_by_vocab[v]);

    for (pass = 0; pass < 2; pass++) {
        len = absent_emitf(out, cap, 0, "{\"%s\":%ld", KEY_READS, g_global_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_OWED, g_owed_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_INDEX, g_index_refused);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_APP, g_appstate_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_TYPEOF, g_unresolved_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_IN, g_in_reads);
        for (v = 0; v < ABSENT_VOCAB_N; v++)
            len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_VOCAB[v], g_owed_by_vocab[v]);
        for (i = 0; i < g_owed_n; i++) {
            const char *k = g_owed[i].name;
#if APICLIENT_DEV
            const char *c;

            /* THE PROVENANCE THIS WRITE RESTS ON, ASSERTED WHERE IT IS RELIED ON. The key is written with no
               escaping, which is sound only because it is a generated table's entry rather than a page's
               bytes — so what is checked is exactly what the write needs and no more: a byte that would end
               the JSON string early, or one a reader would have to decode.
               THE SCAN IS DEV-ONLY AND THE `#if` IS OVER THE LOOP RATHER THAN INSIDE IT, which is this file's
               own shape at names_find and is not tidiness: a DCHECKF body compiles out and the loop AROUND it
               does not, so leaving it would walk every byte of every row of every census a release build ever
               composes in order to do nothing. */
            for (c = k; *c; c++)
                DCHECKF(*c != '"' && *c != '\\' && (unsigned char)*c >= 0x20,
                        "the owed-name census is writing a row key holding a byte JSON cannot carry raw "
                        "(\"%s\", byte 0x%02x) — this row is written with no escaping pass because its key is "
                        "browser/platform_names.h's, browser/language_names.h's or browser/i18n_names.h's OWN "
                        "entry, keyed by pointer "
                        "so a page's bytes can never reach it. A byte like this means that provenance no "
                        "longer holds, and the document embedding this census would not parse",
                        k, (unsigned char)*c);
            DCHECKF(*k != '_',
                    "the owed-name census is writing a row named \"%s\", which opens on the `_` its MEMBERS "
                    "are told apart by — a consumer sums the rows' buckets to get the reads and reads the "
                    "members as the population they are drawn from, so a row in the member namespace puts a "
                    "denominator inside its own numerator. No name Web IDL exposes on Window and no name "
                    "ECMAScript §19 puts on the global object opens on an underscore, so this is a generated "
                    "table having gained a name neither standard spells", k);
#endif
            /* THE ROW IS A HISTOGRAM AND NOT A COUNT, AND EVERY BUCKET IS EMITTED INCLUDING THE ZEROES —
               which is the members' own rule one level down and is load-bearing twice over. It is what makes
               `typeof 3, in 0, read 0` the positive statement that this document guarded the name and never
               read it, rather than a table a reader has to know the shape of; and extension/bridge.js refuses
               an EMPTY histogram on this census by name, so a row that listed only its non-zero buckets would
               abort the trusted zone for every document whose reads all took one entry.
               THE TOTAL IS NOT A FOURTH BUCKET. `reads` is the SUM of these three and printing it beside them
               is CLAUDE.md §EVIDENCE-INFLATION in a JSON object: a reader summing the table would get twice
               the reads, with every bucket still looking like a measurement. The sum IS the total, which is
               what the row identity above asserts and what leaves nothing here to double-count. */
            len = absent_emitf(out, cap, len, ",\"%s\":{", k);
            for (e = 0; e < ABSENT_ENTRY_N; e++)
                len = absent_emitf(out, cap, len, "%s\"%s\":%ld", e ? "," : "", KEY_ENTRY[e],
                                   g_owed[i].by_entry[e]);
            len = absent_emitf(out, cap, len, "}");
        }
        len = absent_emitf(out, cap, len, "}");
        if (pass == 0) {
            cap = len + 1;
            out = (char *)malloc(cap);
            if (!out) return NULL;
        }
    }
    DCHECK(len + 1 == cap,
           "the owed-name census was written to a different length than it was measured for — the two passes "
           "walk one table with one argument list, so they can only disagree if a row changed between them, "
           "and the byte the second pass lost is the closing brace rather than a digit");
    return out;
}

/* THE PUBLISHED RECORDS AND THE PATHS THEY WERE PUBLISHED AT. Keyed by the record's ADDRESS — see the header
   comment for why that is exact and not a heuristic. Newest first on lookup, so that if an address is ever
   recycled between two published records the row that describes the live one is the one found. */
typedef struct { void *obj; char *path; } NsRow;
static NsRow *g_ns;
static int g_ns_n, g_ns_cap;

static const char *ns_path_of(JSValueConst v)
{
    void *p = JS_VALUE_GET_PTR(v);
    int i;

    for (i = g_ns_n - 1; i >= 0; i--)
        if (g_ns[i].obj == p)
            return g_ns[i].path;
    return NULL;
}

void absent_free(void)
{
    int i;

    for (i = 0; i < g_ns_n; i++)
        free(g_ns[i].path);
    free(g_ns);
    g_ns = NULL;
    g_ns_n = g_ns_cap = 0;
    /* AND THE OWED-NAME CENSUS WITH IT, WHICH IS WHAT MAKES IT A PER-AGENT READING RATHER THAN A PER-PROCESS
       ONE. The rows hold no allocation of their own — every `name` is a generated table's static entry — so
       only the vector goes; the counters are zeroed BESIDE it, because a table emptied under counters that
       kept counting would publish an `owed` total that no row sums to and the identity the composer asserts
       would fire on the next census of the next agent. THAT ORDERING IS WHAT THIS COMMENT IS FOR and it holds
       whatever any host does.
       IT SAID PER-DOCUMENT, and named "a host that runs several documents in one process (the native WPT
       runner)" as the boundary at which `_absent` starts again. False when written: that runner builds one
       top-level document per process, and the host that DOES take a second one — the extension's, through
       `qjs_join` — reaches neither an init nor a release, so its two documents share these rows. This file's
       own KIND banner already said `agent`, and it is the sentence that was right.
       THE LAST CLAUSE SURVIVES AND IS CHECKABLE RATHER THAN ASSERTED: this IS the boundary `_orphansDriven`
       uses, because solver_agent_free zeroes that pair and then calls concolic_free, which calls this — one
       function, one boundary (`git grep -n 'absent_free\|concolic_free' -- engine/host`). */
    free(g_owed);
    g_owed = NULL;
    g_owed_n = g_owed_cap = 0;
    g_owed_reads = g_index_refused = g_appstate_reads = g_global_reads = 0;
    g_unresolved_reads = g_in_reads = 0;
    for (i = 0; i < ABSENT_VOCAB_N; i++)
        g_owed_by_vocab[i] = 0;
}

/* THE KEY, READ ONCE FOR EVERY HALF OF THIS FILE, AND THE ENGINE'S GATE ASSERTED WHERE THE PATH IS COMPOSED
   FROM IT. The channel is a server writing a RECORD OF FIELDS, so its keys are strings and array indices —
   and the engine gates on exactly that (JS_AtomIsPublishedName) before it asks any hook. This is the other
   side of that gate: a SYMBOL reaching here would be spelled into a provenance out of its DESCRIPTION, which
   is neither unique nor a name (`Symbol()` twice spells one path for two keys), and a WELL-KNOWN one is the
   engine's own protocol — a slot the interpreter is about to CALL.
   IT IS ASSERTED RATHER THAN FILTERED BECAUSE THE FILTER ALREADY EXISTS AND HAS BEEN GONE AROUND TWICE, and
   the two are worth keeping side by side because they are the same omission at two different altitudes.
   FIRST: the gate arrived with the HIT arm and the MISS arm asked nothing, so for the life of that asymmetry
   §7.1.1 ToPrimitive ( input [ , preferredType ] ) step 1.a's `? GetMethod(input, %Symbol.toPrimitive%)` — a
   read that misses on EVERY object — was answered here with a callable unknown, and step 1.b.vi's "Throw a
   TypeError exception" ended the document (`var b={}; 1 & b` and `1 & globalThis` both died).
   SECOND, AND WORSE, because it produced no exception at all: the WALK that decides which records are on the
   channel asked no key rule either, and a record reached through a key is a record PUBLISHED, not a member
   answered. So an internal-slot record — ECMAScript §6.1.7.2 Object Internal Methods and Internal Slots, which
   this engine holds as named fields on an object hung off a private Symbol
   (engine/host/browser/core/idl_slots.h) — became a namespace, and every one of its well-named fields then
   answered with an unknown through a key rule that passed. An `Event` an inline script left in a `var`
   reported `defaultPrevented` true and `dispatch` set, so DOM §2.7 Interface EventTarget's dispatchEvent(event)
   method step 1 threw InvalidStateError on the first dispatch of a freshly constructed event. THAT is why this
   assert covers the publication and not only the two reads: the publication is where the path is composed, so
   it is where a key that cannot be spelled must crash.
   AND THE KEY RULE WAS NEVER THE WHOLE OF THAT SECOND DEFECT, WHICH IS WHY MEMBERSHIP MOVED TOO. It stopped
   the SLOT RECORD being published; the `Event` itself is reachable under an ordinary string key, so it stayed
   on the channel and a read of a member it does not hold still minted an unknown for a value Web IDL §3.8
   defines. The rule this file's header states — extent GRANTED at the operation that wrote it — is what takes
   a platform object off the channel, and neither rule substitutes for the other: one is about the KEY a record
   is reached by, the other about WHOSE CHOICE its member list was. */
static const char *ns_key_str(JSContext *ctx, JSAtom name)
{
    const char *s;

    DCHECK(JS_AtomIsPublishedName(JS_GetRuntime(ctx), name),
           "the engine asked this channel about a key it cannot NAME — the injected-state channel is a record "
           "of string- and index-keyed fields, so a symbol here is a read or a publication that reached the "
           "hook without going through js_absent_ask / js_present_ask / js_publish_document_namespace's key "
           "rule. A well-known symbol answered with an unknown replaces a slot the interpreter is about to "
           "CALL: §7.1.1 ToPrimitive ( input [ , preferredType ] ) step 1.a reads %Symbol.toPrimitive% off "
           "every object it coerces; a record published under one turns every internal slot behind it into an "
           "unknown, which throws nothing and is read as state");
    s = JS_AtomToCString(ctx, name);
    /* ONE ALLOCATION FAILURE, ONE ANSWER, AND IT IS THE FATAL ONE — because what the other answer produces is
       not a degraded report but a FABRICATED one, and a fabricated answer to exactly these two reads is what
       this whole file exists to prevent. Every caller's only other return is JS_UNINITIALIZED, which the
       engine reads as the positive statement "this read is not on the channel" (js_absent_ask /
       js_present_ask): the MISS arm then completes §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ) step
       2.b's `undefined`, and the HIT arm hands back the slot's own bytes. So a failed malloc answers
       `if (__FLAGS.admin)` concretely, the gate stops forking, and the logged-in surface this tool is for is
       buried — silently, with a smaller result set reported as a clean one, which is the defaulted-field
       defect reached through the allocator instead of through a `||`. The publication half of this same file
       has always spelled that failure `CHECK` (CLAUDE.md §Offensive programming names OOM as CHECK's own
       example: "a dropped flow corrupts the frontier"), and one function cannot hold two answers to one
       failure.
       AND ALLOCATION IS THE ONLY FAILURE IT HAS, which is what makes this a CHECK rather than a judgement
       call: the DCHECK above establishes the atom is a published name, and JS_AtomIsPublishedName defines
       that as a tagged integer or a JS_ATOM_TYPE_STRING atom — neither of which can drive JS_AtomToString
       down its exception path for any reason but a failed allocation. */
    CHECK(s != NULL, "absent: OOM spelling a key of the document's injected-state namespace — the alternative "
                     "to failing here is answering a server-injected read concretely, which decides its gate "
                     "for the whole program and buries the surface behind it with nothing to say so");
    return s;
}

/* HOW A KEY EXTENDS THE PATH IT IS READ AT — the ONE join, because a path composed two ways is two names for
   one unknown the moment either spelling drifts, and every predicate over either would then decide only half
   of them. Both halves of this file compose a path (the publication files one, the two read hooks spell a
   member's provenance out of one) and both went through `%s.%s`.
   AN INDEX IS NOT JOINED WITH A DOT, and that is not cosmetic. A provenance is the EXPRESSION THE RUN BUILT
   (CLAUDE.md §@H), which is what makes it a thing a person can paste and a thing the next candidate can be
   composed onto: `__STATE__.users[0].role` is that expression and `__STATE__.users.0.role` is a syntax error
   wearing a path. It used to be nearly unreachable — an index key arrived only where a server literally wrote
   `{"0":…}` — and it is now the ordinary case, because the engine's walk descends through the LISTS a state
   tree is made of and reaches every element by its index.
   THE ROOT SPELLS `window[0]` AND A BARE NAME. A member of the global namespace is read as `__FLAGS`, which is
   the expression, and `window` is what the same read needs the moment the key is an index: `0` alone names
   nothing, and the run that produced it evaluated `window[0]`.
   The atom decides, never the spelling: ECMAScript §6.1.7 The Object Type makes an array index a canonical
   numeric string, so `x["01"]` is a NAME and a digit test over the key's characters would join it as `x[01]` —
   one path for two different keys. JS_AtomIsIndexName is the engine's own already-made decision.
   The result is the caller's to free. */
static char *ns_join(const char *base, JSAtom name, const char *key)
{
    static const char *const root = "window";
    size_t len = (base ? strlen(base) : strlen(root)) + strlen(key) + 3;
    char *out = (char *)malloc(len);

    CHECK(out != NULL, "absent: OOM composing a path in the document's injected-state namespace — the "
                       "alternative to failing here is a member reported under a name no document published, "
                       "or an injected read answered concretely, which decides its gate for the whole program");
    if (JS_AtomIsIndexName(name))
        snprintf(out, len, "%s[%s]", base ? base : root, key);
    else if (base)
        snprintf(out, len, "%s.%s", base, key);
    else
        snprintf(out, len, "%s", key);
    return out;
}

void absent_publish_hook(JSContext *ctx, JSValueConst parent, JSAtom name, JSValueConst value)
{
    JSValue g = JS_GetGlobalObject(ctx);
    int is_root = (JS_VALUE_GET_PTR(parent) == JS_VALUE_GET_PTR(g));
    const char *base = is_root ? NULL : ns_path_of(parent);
    const char *n = ns_key_str(ctx, name);
    char *path;

    JS_FreeValue(ctx, g);
    /* A CHILD ARRIVING BEFORE ITS PARENT IS THE ENGINE AND THIS FILE DISAGREEING, not a case to default past.
       js_publish_document_namespace walks OUT from the global object and publishes a record before it descends
       into it, so a parent that is neither the global nor a filed row means the walk reached this record by
       some other route than the one this path is composed for — and the composed name would then describe a
       place in the document's namespace that nothing was published at. */
    DCHECK(is_root || base != NULL,
           "a record was published under a parent this file has never filed — the engine's walk publishes a "
           "parent before descending into it, so a missing parent path means the two disagree about what the "
           "published graph is, and every member read off this record would be reported under a name the "
           "document never published it at");
    path = ns_join(base, name, n);
    JS_FreeCString(ctx, n);

    if (g_ns_n == g_ns_cap) {
        int cap = g_ns_cap ? g_ns_cap * 2 : 8;
        NsRow *rows = (NsRow *)realloc(g_ns, sizeof(*rows) * (size_t)cap);
        CHECK(rows != NULL, "absent: OOM growing the published-namespace registry");
        g_ns = rows;
        g_ns_cap = cap;
    }
    g_ns[g_ns_n].obj = JS_VALUE_GET_PTR(value);
    g_ns[g_ns_n].path = path;
    g_ns_n++;
}

/* THE ONE SPELLING of an injected member's provenance, used by both halves of this file.
   The PROVENANCE is the whole read as the run composed it — `gon` and `gon.current_user_id` are two different
   unknowns and each must decide only its own predicates, so the path is composed WHOLE rather than into a
   fixed buffer: a truncated provenance is not a shorter name for one unknown, it is one name for every unknown
   that shares a prefix, and every predicate over any of them would then decide all of them. A server's state
   tree is as deep and as verbosely named as the server chose.
   `base` is the record's published path, or NULL for a member of the global namespace itself; the join is
   ns_join's, which is the same one the publication files a path with. Both outputs are the caller's to
   free. */
static void ns_member_spell(const char *base, JSAtom name, const char *key, char **shape, char **src)
{
    char *path = ns_join(base, name, key);
    size_t n = strlen(path) + 3;

    *shape = (char *)malloc(n);
    CHECK(*shape != NULL, "absent: OOM spelling the provenance of an injected member");
    snprintf(*shape, n, "{%s}", path);
    *src = path;
}

/* A MEMBER THE PUBLISHED RECORD HOLDS — see this file's header for why that is the same unknown as one it does
   not, and the header of JSConcolicHooks.present for which base the engine asks and why it is not the read
   hook's. The value the slot holds becomes the EXAMPLE, so the flow keeps forking on the gate over it AND the
   report keeps the bytes the server sent; the mint goes through concolic_new like every other source read, so
   an @S candidate substitutes at `__FLAGS.admin` exactly as it does at a member nothing wrote. */
/* WHAT THIS HOOK COSTS, AND THE HALF OF ITS OWN PREMISE THE CODE DOES NOT ASK — recorded here because the
   premise is stated in JSConcolicHooks.present/.publish's own contract and the implementation serves a wider
   population than the channel that contract names — which is the one shape a reader stops checking, because
   the paragraph they arrive at is right.
   THE CHANNEL IS NAMED IN quickjs.h'S OWN WORDS AS "the document's INLINE half handing a RECORD to its
   EXTERNAL half, which is the one channel a server has for injecting per-visitor state into a bundle it ships
   unchanged to everybody". Membership is decided by js_publish_document_member, which admits any container an
   INLINE script built and left reachable from the global. That is every top-level `var x = {...}` of every
   inline `<script>`, including the ones a script uses as its OWN WORKING STATE and reads back three lines
   later — and this hook then answers those reads with an unknown, so a comparison over a value THIS RUN
   COMPUTED forks. §Solver-half makes concrete execution the ground truth; the inline half reading its own
   record is not the inline->external channel and there is no server choice in it.
   THE COST IS FAN-OUT AND IT IS THE FRONTIER'S FIRST CAUSE, NOT A ROUNDING TERM. A `&&` chain of N such
   comparisons fans 2^N per arriving arm, every arm is born holding a live frame, and §Offensive-programming's
   whole task ladder in solver/engine.c sits under `if (!f->frame)` — so the arms this manufactures can run no
   job, take no reply and start no program until each one individually finishes the program it was forked
   inside. Measured at the artifact stamped head 1d666bda (dirty []), on the smoke: the four heaviest fork
   sites are `mo.threw`, `mo.ran`, `vo.threw`, `vo.ran` at 125 forks each — 500 of 761, TWO THIRDS — and all
   four are members of two object literals the same inline program assigns five lines above the comparison.
   THE DERIVATION, so the next reader gets today's set rather than this sentence:
     grep '^@FORKAT ' <smoke log> | tail -1   — the Space-Saving fork census, keyed by predicate; a key
     spelled `s<len>:<path>` with no leading `1:.` derivation is a bare SOURCE, and a source whose path is a
     record an inline script built is one of these.
   Read it against @COLD's `live`/`framed` on the same census: this hook's output is what makes those two
   numbers converge.
   THE CHEAP HOST-SIDE NARROWING IS WRONG AND IS REFUTED HERE SO IT IS NOT RE-DERIVED. Asking the SOLVER which
   row the running flow compiled (flow_dyn_kind: DYN_PAGE_SCRIPT is inline, DYN_SCRIPT_SRC is external) answers
   about the TOP-LEVEL PROGRAM and not about the code executing, and quickjs.h says the property rides the
   whole nest — "a function declared inside an inline script is inline-script code too, and a DIRECT eval
   inherits it from its caller". A bundle function CALLED from an inline program would then read as inline and
   its `__FLAGS.admin` would concretize, which is the exact loss the channel exists to prevent. The
   discriminator is the READER's JS_EVAL_FLAG_INLINE_SCRIPT, which only the engine holds.
   AND THAT NARROWING IS NOT ESTABLISHED EITHER — its counterexample is two inline scripts of one document,
   one writing `window.__FLAGS={admin:false}` and the next reading it, which is an ordinary SSR shape and would
   concretize under it. So the question is open rather than answered, and it is stated as a question.
   THE ORDERED SUBPROBLEMS, WITH THE CALL THAT CONSUMES EACH. (1) Decide whether an inline reader is off the
   channel, against the two-inline-scripts counterexample above — consumed by js_present_ask/js_absent_ask in
   the SUBMODULE, so it is a cross-boundary diff and not a solver one. (2) If it is, carry the reader's
   inline-script flag to both ask sites and add it as a conjunct — consumed by the same two call sites, and the
   ABI change and this file's arm land together or not at all (§A-CROSS-BOUNDARY-DIFF). (3) If it is not, the
   fan is legitimate and what must change is the frontier's ability to carry it,
   which is CLAUDE.md §ONE-WFQ-POLICY's stated open question about what mints an ACCOUNT — consumed by
   flow_fork_inherit, and explicitly NOT by a weight term, which fork rank-neutrality already refuses. */
JSValue absent_present_hook(JSContext *ctx, JSValueConst holder, JSAtom name, JSValueConst value)
{
    const char *s = ns_key_str(ctx, name);
    const char *base;
    char *shape = NULL, *src = NULL;
    JSValue r = JS_UNINITIALIZED;

    DCHECK(JS_VALUE_GET_TAG(value) != JS_TAG_OBJECT,
           "the engine asked about an OBJECT-valued member of a published record. A record hanging off a "
           "published record is published in its OWN right by the same walk, and its address is this file's "
           "registry key — minting a fresh unknown per read would answer `gon.user === gon.user` false and "
           "hide the key behind a value nothing filed");
    base = ns_path_of(holder);
    /* THE ENGINE HAS ALREADY DECIDED THIS RECORD IS PUBLISHED — the mark is set only by the walk that files
       the row — so a record with no path is the mark and the registry disagreeing, exactly as it is on the
       miss path, and the alternative to crashing is a member reported under a name no document published. */
    DCHECK(base != NULL,
           "a read hit a member of a record the engine says the document published, and this file holds no "
           "path for it — without the path this member would be reported as a bare field name that any other "
           "namespace's identically-named field is indistinguishable from");
    if (!base)
        goto done;
    ns_member_spell(base, name, s, &shape, &src);
    r = concolic_new(ctx, shape, src, JS_DupValue(ctx, value));
done:
    free(shape);
    free(src);
    JS_FreeCString(ctx, s);
    return r;
}

/* THE SAME MISS ON THE GLOBAL, REACHED BY AN OPERATOR THAT PERFORMED NO [[Get]] — of which there are TWO,
   and `op` is the engine stating which rather than this file inferring it from a name that cannot say.
   RECORDING ONLY, and the void return is the contract for both rather than a convenience, because neither
   operator's algorithm contains a read for a host to answer. ECMAScript §13.5.3 The typeof Operator's
   §13.5.3.1 Runtime Semantics: Evaluation step 2.a is "If IsUnresolvableReference(value) is true, return
   "undefined".", which settles that operator BEFORE step 2.b's GetValue — so a value handed back would make
   step 2.b run and make it say "object" where every browser says "undefined". §13.10.1 Runtime Semantics:
   Evaluation's `RelationalExpression : RelationalExpression in ShiftExpression` ends at "Return ?
   HasProperty(rightValue, ? ToPropertyKey(leftValue))" — §7.3.11 HasProperty ( obj, propertyKey ), an
   internal method that is not §7.3.2 Get — so a value handed back there would be an answer to a question the
   standard does not ask. The engine calls this on its way past each arm and decides nothing.
   WHY IT IS THE SAME CENSUS AND NOT A SECOND ONE: `window.EventSource`, `typeof EventSource` and
   `"EventSource" in window` are ONE question about ONE name, and this file's whole answer is which names a
   document asked a standard for and this realm did not answer. Three censuses would be three rows for one
   name, and a reader summing any of them would have a number about which OPCODE the bundle happened to use.
   So the classification is global_miss_note's, the row is the same row, and what separates the entries is a
   MEMBER of the object rather than a second object.
   THE KEY RULE IS THE ENGINE'S AND IS NOT ONE RULE, which is why this file may assert it here for both. The
   `typeof` atom is an identifier's, which the grammar makes a string atom; the `in` key is the page's own
   `ToPropertyKey(leftValue)` and may be a SYMBOL, so js_absent_note_in FILTERS it before this file is spoken
   to at all — CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE, since an assert on that one would be a page-held abort
   switch reached from a feature detect. By the time either arm arrives here the key is nameable for a reason
   of its own, which is what ns_key_str's DCHECK states and what makes it a check rather than a filter. */
void absent_unresolved_note(JSContext *ctx, JSAtom name, JSConcolicAbsentOp op)
{
    const char *s = ns_key_str(ctx, name);

    /* RAISED BEFORE THE CLASSIFICATION AND NOT INSIDE IT, so a cut and the denominator are raised at two
       different events and the composer's containment asserts have two sides that can actually disagree.
       THE SWITCH HAS NO `default` AND THE ENUM IS CLOSED, so a third operator gaining a member of
       JSConcolicAbsentOp is a COMPILE diagnostic here rather than a read that silently raises no cut and
       leaves the census reporting a smaller absence than the document asked for. That is the direction this
       whole file is about: a number that falls because the instrument stopped looking. */
    /* AND THE SAME SWITCH MAPS THE ENGINE'S OPERATOR ONTO THIS FILE'S ENTRY, which is the whole of the
       translation between the two enums and is why there is exactly one of it. `entry` opens on the POISON
       rather than on a member, so the no-`default` property above gains a RUNTIME backstop to go with its
       compile diagnostic: a member added to JSConcolicAbsentOp breaks this build, and a build that somehow
       proceeds anyway aborts at owed_note's own assert with the name in hand instead of filing the new
       operator's reads under READ and reporting an unguarded read for a guarded one. */
    AbsentEntry entry = ABSENT_ENTRY_N;

    switch (op) {
    case JS_CONCOLIC_ABSENT_TYPEOF: g_unresolved_reads++; entry = ABSENT_ENTRY_TYPEOF; break;
    case JS_CONCOLIC_ABSENT_IN:     g_in_reads++;         entry = ABSENT_ENTRY_IN;     break;
    }
    /* THE ANSWER IS DISCARDED AND THAT IS THE WHOLE DIFFERENCE FROM THE READ HOOK: the arms decide what the
       READ hook returns, and this entry has already been answered by the operator. Every arm's counter and
       every row is raised inside, so there is nothing here left to do with the verdict. */
    (void)global_miss_note(s, name, entry);
    JS_FreeCString(ctx, s);
}

JSValue absent_read_hook(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    JSValue g = JS_GetGlobalObject(ctx);
    int is_global = (JS_VALUE_GET_PTR(obj) == JS_VALUE_GET_PTR(g));
    const char *s = ns_key_str(ctx, name);
    const char *base = NULL;
    JSValue r = JS_UNINITIALIZED;
    char *shape = NULL, *src = NULL;

    JS_FreeValue(ctx, g);
    if (is_global) {
        /* WHAT THE NAME WAS, ANSWERED WHERE BOTH ENTRIES ANSWER IT. The three arms, their counters and the
           denominator are global_miss_note's, because the classification is a question about the NAME and
           the same for a read this hook answers and for a `typeof` the engine already answered; what is left
           HERE is the only part that differs, which is what to return. `1` is "the read is left alone" — a
           standard owes the name so its throw names the component, or the key is an integer on a global that
           is not a list — and JS_UNINITIALIZED is the positive statement the engine reads as "this read is
           not on the channel", which completes §10.1.8.1 OrdinaryGet ( O, P, Receiver ) step 2.b's
           `undefined`. Falling through is the third arm: no vocabulary claimed the name, so it is
           server-injected app state and the mint below is what this entry has that the other does not. */
        if (global_miss_note(s, name, ABSENT_ENTRY_READ))
            goto done;
    } else {
        base = ns_path_of(obj);
        /* THE ENGINE HAS ALREADY DECIDED THIS RECORD IS PUBLISHED — it does not ask otherwise — so a record
           with no filed path is the mark and the registry disagreeing, and the alternative to crashing is a
           member reported under a name no document published. */
        DCHECK(base != NULL,
               "a read missed on a record the engine says the document published, and this file holds no path "
               "for it — the mark is set only by the walk that files the row, so the two cannot come apart "
               "unless a row was dropped; without the path this member would be reported as a bare field name "
               "that any other namespace's identically-named field is indistinguishable from");
        if (!base)
            goto done;
    }
    /* Example-free, and that is the ONE way this half differs from the present half: nothing here knows what a
       logged-in visitor's flags WOULD hold, and inventing one fabricates an observation. The provenance is
       spelled by the same speller either way — see ns_member_spell. */
    ns_member_spell(base, name, s, &shape, &src);
    r = concolic_new(ctx, shape, src, JS_UNDEFINED);
done:
    free(shape);
    free(src);
    JS_FreeCString(ctx, s);
    return r;
}
