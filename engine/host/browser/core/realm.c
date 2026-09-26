/* See realm.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/idl_args.h"   /* §3.3.7 [Exposed] step 1: resolving a [Global] interface's global names */
#include "core/realm.h"
#include "solver/concolic.h"
#if APICLIENT_DEV
/* THE GENERATED CORPUS TABLE ITSELF, read for the check below and not restated. This is the second
   translation unit to include it (core/idl_args.c is the first), and that is the point rather than a
   duplication: an auditor of a rule derives the rule from the artifact the rule is about, so the set of
   identifiers §3.8 defines on a global is asked of the same generated rows the install path asks. A
   hand-kept list here would be a third copy of a fact whose second copy is what this check exists to catch.
   Under APICLIENT_DEV because the walk below is, and an unused static table at DEV=0 is a diagnostic. */
#include "idl_exposure.h"
#endif

static RealmIntrinsic *g_list;
static int g_n, g_cap;
/* HAS ANY REALM RUN THE LIST YET? The list is CLOSED at the first realm, and this is what says so. It is not
   under `#if APICLIENT_DEV` because check.h's release DCHECK keeps `(void)sizeof(cond)`, so every identifier a
   dev-only assertion names must still be DECLARED in a release translation unit — the same asymmetry
   core/idl_args.c states over its two global-member helpers, and the reason both builds are checked. */
static bool g_realm_built;

void realm_declare_intrinsic(RealmIntrinsic install)
{
    int i;

    DCHECK(install != NULL, "a component declared a per-realm intrinsic with no install to run");
    /* AND IT WAS DECLARED BEFORE ANY REALM EXISTED. Every realm runs `g_list` AS OF THE MOMENT IT IS BUILT, so
       this is the one invariant that decides whether two realms of one agent get the SAME platform surface. */
    DCHECK(!g_realm_built,
           "a per-realm intrinsic was declared AFTER a realm in this agent had already been built. Every "
           "realm runs this list as of the moment IT is built, so a declaration that lands late gives two "
           "realms of one agent DIFFERENT lists: the earlier realm is missing a construct the later one has, "
           "and no member answers wrongly anywhere — the construct is simply ABSENT from a realm Web IDL "
           "§3.3.7 \"[Exposed]\" step 1 says it is exposed in, which is the one direction this file's own "
           "§3.8 walk reads as a defect in the component rather than in the ORDER. THIS HELD BY CONVENTION "
           "UNTIL THIS LINE AND NOW HOLDS BY CONSTRUCTION: core/platform.c runs its whole declare column and "
           "only then calls realm_install_intrinsics, and every realm_declare_intrinsic in "
           "engine/host/browser sits in a declare-column function — which was a hand reading of a hundred and "
           "fifty call sites, and a reading cannot cover the site somebody adds next. THE FIX IS NEVER TO "
           "DECLARE LATER: a component whose per-realm object depends on something built at run time still "
           "declares HERE with the others and asks its own question INSIDE its install, the way "
           "core/workers/worker_global_scope.c asks §3.8's exposure step at the head of its own");
    for (i = 0; i < g_n; i++)
        DCHECK(g_list[i] != install,
               "a per-realm intrinsic was declared twice — it would then build its prototype twice in every "
               "realm, and the second build leaves everything already chained to the first answering out of a "
               "discarded object");
    if (g_n == g_cap) {
        int cap = g_cap ? g_cap * 2 : 8;
        RealmIntrinsic *g = realloc(g_list, (size_t)cap * sizeof *g);
        CHECK(g != NULL, "realm: OOM recording a per-realm intrinsic — a realm missing one answers a member "
                         "out of another document");
        g_list = g;
        g_cap = cap;
    }
    g_list[g_n++] = install;
}

/* HTML §8.1.3.1's TOP-LEVEL CREATION URL, in the per-realm store this file already owns. `JS_INVALID_CLASS_ID`
   is the "not declared" value because it is quickjs's own reserved class id and therefore the one value
   realm_value_declare can never hand back, so there is one sentinel rather than two.
   THIS USED TO SAY "zero ... because it is also the invalid slot realm_value_set asserts against", which was
   true of an assert that is gone: that entry no longer asks whether the number is positive, it asks the
   RUNTIME whether the class was minted. The conclusion is unchanged and its reason is not, and the reason is
   rewritten rather than deleted because a reader who re-derives the old one will write a second sentinel back.
   The DECLARATION is per AGENT (a slot is a class id, which belongs to a runtime), so it happens on the first
   realm and is released with the agent below. */
static JSClassID g_top_level_url_slot;

/* WEB IDL §3.3.8 [Global]'s GLOBAL NAMES OF THIS REALM, in the same per-realm store and for the same reason as
   the URL above: it is a fact about the ENVIRONMENT, it is settled when the realm is built, and §3.3.7 step 1
   is asked of it while the intrinsics are still installing. Resolved to the generated bit set at the call
   rather than at each ask, so a host names an interface and nothing downstream re-derives what that name
   means. */
static JSClassID g_global_names_slot;

/* HTML §8.1.3.5 "Secure contexts" STEP 1.2.1's OPERAND — the answer the worker's owner gave — in the same
   per-realm store and for the same reason as the two above: it is a fact about the ENVIRONMENT, settled when
   the realm is built, and step 1.2.1 is asked of it while the intrinsics are still installing (every
   [SecureContext] member of every per-realm install runs §3.3.7 step 2 through it). WRITTEN ONLY FOR A WORKER
   REALM, because it is the only realm the step reaches; see realm.h for why the fact crosses as a boolean. */
static JSClassID g_owner_secure_slot;

#if APICLIENT_DEV
/* bsearch over IDL_EXPOSURE's own rows, keyed by the identifier — the same shape core/idl_args.c uses over the
   same table, because the table is sorted by that field and there is one right way to ask it. */
static int idl_exposure_row_name_cmp(const void *key, const void *row)
{
    return strcmp((const char *)key, ((const IdlExposureRow *)row)->name);
}

/* bsearch over a plain sorted array of names — browser/idl_exposure.h's IDL_PROTOTYPE_ONLY, and the same shape
   core/idl_args.c uses over an IDL_GLOBALS row's `own` band. The two are not shared because the natural home
   for one spelling is core/idl_args.h and this diff does not touch that header; they are one line each and
   both are checked against arrays engine/idlgen.mjs refuses to emit unsorted or non-ASCII, which is what makes
   two spellings safe here rather than the shape that drifts. */
static int idl_name_ptr_cmp(const void *key, const void *el)
{
    return strcmp((const char *)key, *(const char *const *)el);
}

/* WEB IDL §3.8 Platform objects implementing interfaces' DESCRIPTOR, ASSERTED OVER THE FINISHED GLOBAL — the
   one check that can see an install which never called the install entry.
   WHY IT CANNOT LIVE AT THE ENTRY. `define the global property references` has exactly one door here
   (core/idl_args' idl_define_global_property_reference), and the failure this guards is a component that does
   not USE that door: it reaches for JS_SetPropertyStr, which is ECMAScript §10.1.9.2 OrdinarySetWithOwnDescriptor
   creating the property through CreateDataProperty, whose descriptor is writable AND ENUMERABLE AND
   configurable. §3.8 performs "DefineMethodProperty(target, id, interfaceObject, false)" and ECMAScript
   §10.2.8 DefineMethodProperty ( homeObj, name, closure, enumerable ) writes the triple down — "Let
   propertyDesc be the PropertyDescriptor { [[Value]]: closure, [[Writable]]: true, [[Enumerable]]: enumerable,
   [[Configurable]]: true }" — so with §3.8's `false` substituted the two agree on the value and disagree on
   [[Enumerable]]. An assert INSIDE the entry is blind to every caller that never arrives at it, and would in
   any case stamp the entry's own file and line for all hundred-odd callers, naming an action with no object.
   THE IDENTIFIER IS THE SITE. This does not thread a caller's __FILE__ anywhere: the message names the
   IDENTIFIER, and an identifier is an address — `git grep '"Screen"' engine/host/browser` is one command and
   lands on the line that installed it. That is what makes a walk over a finished object an actionable crash
   rather than a report that something, somewhere, is wrong.
   WHY IT IS SOUND. The subject is a realm this codebase has just finished building, before one byte of page
   script has run, so every property name it reads is a name this codebase wrote — never a value a stranger
   stated, which is the only thing that would make an abort here a switch somebody else holds. The two sets
   cannot collide by accident either, and the reason is STRUCTURAL rather than a property of today's corpus: the
   table is keyed by the identifiers §3.8 puts on a global, so a §3.7.6 attribute or §3.7.7 operation of the
   [Global] interface — which §3.7.6 requires to be ENUMERABLE, and which is spelled `location`, `document`,
   `fetch` — HAS NO ROW AT ALL and bsearch answers NULL for it. Case is corroboration and not the argument, and
   it is deliberately not written here as a count: most of these identifiers are capitalised, some are not
   (`console`, a §3.13.1 namespace object; a §3.7.2 [LegacyWindowAlias] such as `webkitURL`), every one of the
   lower-case ones IS in this band and answers to the same descriptor, and the set moves whenever the corpus is
   regenerated — `grep '^[a-z]' <(…IDL_EXPOSURE identifiers…)` is the derivation, and a number written here
   would be a claim about a generated file another diff owns. No ECMAScript intrinsic global name is a Web IDL
   identifier either, so the JS engine's own globals are outside the set.
   WHAT IT DOES NOT SEE, named because a check trusted past its evidence is worse than none: an interface
   object a HOST puts on the global AFTER the platform's own installs finish (a harness global, a solver seam)
   is outside the walk. Those are not §3.8 property references and none of them is named by the corpus; the day
   one is, this check is the thing that has to move rather than the thing that was wrong.
   AND IT IS ASKED AT BOTH ENDS OF A REALM'S CONSTRUCTION, WHICH USED TO BE ONE END AND A WRONG CLAIM. It ran
   only where the per-realm intrinsic list finishes, under a comment saying the invariant is about the FINISHED
   global — an argument that is exactly right and was being applied to the wrong operand, which is the failure
   a well-argued banner is most likely to hide. A realm whose global object is a WorkerGlobalScope IS finished
   there; a WINDOW realm is not, because core/platform.c's per-document install column runs afterwards, and at
   the revision this was written that column placed 82 of the 143 §3.8 property references in this tree. So the
   walk was auditing under half of a Window's global and reporting it as the whole. Moving it to the other end
   would have been the mirror of that — blind to every worker realm, which is the realm kind §3.3.7 [Exposed]
   step 1 exists to make different — so core/platform.c calls it as well, and this stays where a realm that
   never reaches a document install is still judged. Running twice over one global costs nothing: the walk
   allocates no state, decides nothing, and the properties it asserts are monotone.
   AND IT ASKS BOTH HALVES OF ITS OWN MESSAGE, WHICH USED TO BE ONE HALF AND A CLAIM. The enumeration was
   `JS_GPN_ENUM_ONLY`, so the walk saw ONLY the enumerable failure — the site that reaches for JS_SetPropertyStr
   — while the crash it raised went on to say that such an interface "is ALSO present in every realm its
   exposure set excludes". That second sentence is about §3.3.7 [Exposed] step 1 and the walk was not asking it:
   a site that bypasses §3.8's door with JS_DefinePropertyValueStr and the RIGHT descriptor skips step 1 exactly
   as loudly and is INVISIBLE to an enumerable-only walk. An assert whose message claims more than the check
   performs is the one failure this instrument cannot afford, because it is read as the ground truth for §3.8.
   So the enumeration is now every own string-keyed property and the row decides which QUESTION is owed:
   `JS_GPN_SET_ENUM` fills in `is_enumerable` per property (quickjs.h declares that as the flag's whole
   purpose), the descriptor half is asserted from that field, and the exposure half is asked of
   idl_exposed_in_realm — §3.3.7 step 1's ONE statement, asked ON BOTH SIDES OF THE ROW SINCE §3.3.7's subject
   list is "An interface, callback interface, namespace, or member construct", through idl_exposed_in_realm
   for a row-bearing identifier and through core/idl_args' idl_member_exposed_in_realm for a member — never
   re-spelled here as `row->set & global names`,
   which is why this pays for a second bsearch it could have avoided. core/idl_args.h states that rule from the
   other side: an auditor that spelled the condition itself would be a second copy of the standard, and two
   right answers to one question is the shape that drifts.
   THE ROW IS THE DISCRIMINATOR BETWEEN AN INTERFACE OBJECT AND A [Global] MEMBER, and it is DERIVED. Widening
   past ENUM_ONLY brings in the §3.7.6 attributes and §3.7.7 operations of the [Global] interface — `document`,
   `location`, `alert`, `matchMedia` — which §3.7.6 requires to be ENUMERABLE and which would fail the
   descriptor half if they were in this population at all. They are not, and nothing here decides that:
   IDL_EXPOSURE is keyed by "the identifier Web IDL §3.8 `define the global property references` puts on a
   global", so a MEMBER has no row by construction and bsearch answers NULL for it. The two bands cannot
   collide by accident either, and that is settled by the paragraph above STRUCTURALLY — a member has no row —
   with one further fact this widening newly leans on: the intersection of the corpus with ECMAScript §19's
   global object properties is EMPTY, so the JS engine's own intrinsics, every one of them non-enumerable and
   none of them through §3.8's door, cannot be mistaken for a property reference. Under ENUM_ONLY that was
   inert, because an intrinsic is not enumerable and the walk never saw one; it is load-bearing now, it is a
   fact about two generated vocabularies rather than about this file, and it is re-derived by intersecting
   IDL_EXPOSURE's identifiers with §19's list rather than by trusting this sentence.
   NAMED RESIDUAL — THE PRECONDITION IS CARRIED BY NOBODY. WHAT IS NOT COVERED: the soundness argument above —
   that every property name this reads is a name this codebase wrote — is a fact about WHEN it is called, and this
   function cannot see it. Both callers are inside a realm's construction, before one byte of script has run in
   the realm being walked — but that is a sentence about the call graph, which is exactly the shape a privilege
   may not be justified by. A page CAN add own properties to its own global: an ordinary [[Set]] makes an
   ENUMERABLE one, which is the descriptor half's pre-existing population and not something this widening
   added, while `Object.defineProperty(globalThis, id, {value: 0})` makes a NON-enumerable one and would reach
   the exposure half. Either way a page would hold an abort switch over the engine, and only unreachability
   stops it. WHAT THE NEXT DIFF BUILDS: a per-realm fact recording that script has begun in this realm — none exists today,
   and `quantum_slice_open` is NOT it, because a child navigable's realm is legitimately constructed inside a
   parent's open slice, so asserting it here would fire on a correct case — set where a realm first runs a
   classic script and DCHECKed false at the top of this walk. HOW ITS ABSENCE WOULD SHOW: this walk gains a
   caller outside realm construction, and the engine then aborts on an identifier NO component in this tree
   installs, so `git grep '"<identifier>"' engine/host/browser` lands only in the generated table — a crash
   whose own remedy has no site is the tell that the name came from outside. */
void realm_assert_global_property_references(JSContext *ctx)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    JSValue global = JS_GetGlobalObject(ctx);
    int ok = JS_GetOwnPropertyNames(ctx, &tab, &n, global, JS_GPN_STRING_MASK | JS_GPN_SET_ENUM);

    /* This block only exists in a dev build, so the DCHECK is live here and the failure arm below it is not a
       path: a non-zero status aborts, and `tab`/`n` are never read out of a failed call. */
    DCHECK(ok == 0, "the realm's global could not be enumerated to check Web IDL §3.8's descriptor");
    for (i = 0; i < n; i++) {
        const char *name = JS_AtomToCString(ctx, tab[i].atom);
        const IdlExposureRow *row;

        /* A SKIPPED NAME IS A HOLE IN A CENSUS, so this is an abort and not a `continue`. The walk's whole
           worth is that its population is EVERY own property of the finished global; a name quietly dropped
           because it could not be materialized leaves the audit reporting clean about a property it never
           looked at, which is the shape this instrument exists to catch happening to the instrument. */
        DCHECK(name != NULL,
               "a property name of the realm's global could not be materialized as a string, so Web IDL §3.8's "
               "descriptor could not be asked of it — the only way this fails is allocation");
        row = bsearch(name, IDL_EXPOSURE, sizeof IDL_EXPOSURE / sizeof IDL_EXPOSURE[0],
                      sizeof IDL_EXPOSURE[0], idl_exposure_row_name_cmp);
        /* NO ROW MEANS THIS IS NOT A §3.8 PROPERTY REFERENCE — a §3.7.6 attribute or §3.7.7 operation of the
           [Global] interface, an ECMAScript intrinsic, or a name a host added — so none of those owes the
           DESCRIPTOR question below, which is §3.8's alone. A MEMBER does owe §3.3.7 [Exposed] step 1, and
           that is asked INSIDE this branch rather than below it, because the branch is where the members are.
           See the banner for why the classification is the generated table's to make. */
        if (row == NULL) {
            /* AND A PASSING WALK OVER IDL_EXPOSURE ALONE IS NOT COVERAGE, WHICH IS WHY THIS BRANCH IS NOT A
               BARE `continue` ANY MORE. That table is keyed on the identifiers §3.8 `define the global property
               references` puts on a global, so a MEMBER cannot have a row in it — not because none was written
               but because the table is not about members at all. Every §3.7.6 attribute and §3.7.7 operation
               therefore reached this line, was classified as "not our question", and left; the walk reported
               clean about a population its own predicate could not contain. That is a blind spot wearing a
               check's grammar, and the next reader of a green run will read it as coverage unless it says so
               here.
               IDL_PROTOTYPE_ONLY IS THE BAND THAT CAN SEE IT, and it is a fact about the CORPUS rather than
               about this realm: a name no §3.3.8 [Global] interface declares is one §3.8 never writes onto any
               global, so it is wrong here whatever kind of global this is. That is why nothing below asks for
               a row, a mask or a [Global] identifier — the question has no realm in it, which is also what
               makes it askable in a worker realm, where `importScripts` and `fonts` are two of the six.
               IT IS A `DCHECK` AND ITS OPERAND IS OURS. The name came off a global this codebase finished
               building, before one byte of script ran in it — the same precondition the two asserts below
               stand on, and the same one the banner names as carried by nobody. */
            DCHECKF(bsearch(name, IDL_PROTOTYPE_ONLY, COUNTOF(IDL_PROTOTYPE_ONLY),
                            sizeof IDL_PROTOTYPE_ONLY[0], idl_name_ptr_cmp) == NULL,
                    "`%s` is an OWN property of this realm's global and Web IDL §3.8 Platform objects "
                    "implementing interfaces never writes it onto one. Its [Global] arm defines the members "
                    "of the object's PRIMARY interface — §3.7.7 Operations fixes that set as \"the list of "
                    "regular operations that are members of definition\" — and no §3.3.8 [Global] interface "
                    "in the corpus declares `%s`, so the only algorithm that could place it is §3.7.3 "
                    "Interface prototype object, onto the prototype of whichever interface DOES declare it. "
                    "The one place §3.8 walks the object's chain copies the [[Unforgeables]] slot and nothing "
                    "else, and this name is not in one. A page sees the difference: "
                    "`Object.getOwnPropertyDescriptor(globalThis, '%s')` answers a descriptor here and "
                    "`undefined` in every browser. Install it on the declaring interface's prototype instead: "
                    "`git grep '\\\"%s\\\"' engine/host/browser` finds the site", name, name, name, name);

            /* AND WEB IDL §3.3.7 [Exposed] STEP 1 OF THE *MEMBER*, WHICH IS THE TWIN OF THE EXPOSURE ASSERT IN
               THE ROW-BEARING ARM BELOW AND HAD NO SITE UNTIL NOW. §3.3.7 names its own subjects — "An
               interface, callback interface, namespace, or member construct is exposed in a given realm" — so
               the step is owed by a MEMBER exactly as by an interface object, and the only reason the arm
               below could not ask it is that its operand is IDL_EXPOSURE, whose key is the identifier §3.8
               puts on a global. A member has no row there, so every §3.7.6 attribute and §3.7.7 operation
               arrives HERE and was asked only the realm-independent IDL_PROTOTYPE_ONLY band above.
               THE DIRECTION IS `PRESENT ⇒ PERMITTED` AND THAT IS WHY IT IS ASKABLE OVER AN OUTCOME AT ALL.
               core/idl_args.h's rule is that an invariant over a GATED operation censuses the ASK, because a
               census of what LANDED cannot tell a component that never asked from one the gate correctly
               refused. The gate here is §3.7.6's "If attr is not exposed in realm, then continue." and
               §3.7.7's "If op is not exposed in realm, then continue.", and a refusal is an ABSENCE: the
               member is not placed, so it is not a property, so this walk never enumerates it. An
               `owed ⇒ present` census would fire on every Window-only member in a worker realm; this one
               cannot fire on a refusal because a refusal contributes nothing to the population. What it can
               see is the opposite and is the thing no ask-side check reaches — a member that got here without
               being asked.
               ITS POPULATION IS SAFE BECAUSE THE SILENCE IS, not because this branch is filtered. A name with
               no IDL_MEMBER_EXPOSURE row is EXPOSED (§3.3.7's `*`, and core/idl_args.h states why the silent
               direction is the sound one), and every ECMAScript intrinsic, every `__apiclient*` operand
               binding and every fixture host surface is exactly such a name — none of them is in any of the
               four generated bands. So this asserts a POSITIVE over a population it does not control, and it
               is sound only because the predicate answers TRUE for everything outside the corpus. That is the
               whole reason §3.8's own-property band is not also asked here: `idl_realm_global_declares` answers FALSE for
               `Object`, for `Math` and for every host name, so asking it in this branch would abort on the
               first intrinsic the walk reaches. See the residual below for the operand that question needs.
               IT IS THE MEMBER HALF OF WHAT core/idl_args.c's idl_global_member_refused ASKS AT THE DOOR, and
               the two are not a second copy: that one is the ASK and runs in release because §3.7.6's
               continue-step is the standard's own behaviour, this one is the OUTCOME and exists in a dev build
               only, and neither can see what the other sees. A member placed with a raw JS_SetPropertyStr
               reaches no door at all, which is the population this line is for.
               NAMED RESIDUAL — §3.8's OWN-PROPERTY BAND IS STILL NOT ASKED OF A MEMBER. WHAT IS NOT COVERED: a member
               that IS exposed in this realm under §3.3.7 step 1 and that this realm's own §3.3.8 [Global]
               interface does not DECLARE — §3.8 takes one arm for a global, "If interface is declared with
               the [Global] extended attribute, then: Define the regular operations of interface on instance,
               given realm", and §3.7.7 fixes that set as "the list of regular operations that are members of
               definition", so an INHERITED member belongs on §3.7.3's interface prototype object and never as
               an own property. IDL_PROTOTYPE_ONLY sees only the members NO [Global] interface declares, so a
               member declared by ANOTHER [Global] interface passes both checks here. WHAT THE NEXT DIFF
               BUILDS: the chain-wide union of every [Global] interface's members, re-emitted by
               engine/idlgen.mjs with a reader — browser/idl_exposure.h's own foot names this walk as the
               consumer that would, and says the union is deliberately not emitted until one exists — used as
               the POPULATION FILTER that the per-realm band check is missing: a name in the union but not in
               this realm's IDL_GLOBALS row is the defect, and a name outside the union is an intrinsic or a
               host surface and owes nothing. HOW ITS ABSENCE WOULD BE OBSERVED: a global object answers
               `Object.getOwnPropertyDescriptor(globalThis, id)` with a descriptor for a member the realm's
               own [Global] interface does not declare, where a browser answers `undefined` because the member
               is reached up the prototype chain — and no assert in this file speaks, because step 1 answers
               true for it and IDL_PROTOTYPE_ONLY does not contain it. */
            DCHECKF(idl_member_exposed_in_realm(ctx, name),
                    "`%s` is an own property of this realm's global and Web IDL §3.3.7 [Exposed] step 1 says "
                    "it must not be — \"If construct's exposure set is not `*`, and realm.[[GlobalObject]] "
                    "does not implement an interface that is in construct's exposure set, then return "
                    "false\" — and §3.3.7 asks that of a MEMBER as much as of an interface, its own subject "
                    "list being \"An interface, callback interface, namespace, or member construct is exposed "
                    "in a given realm\". So a browser answers `'%s' in globalThis` FALSE for a global object "
                    "of this kind. The one place that step is asked of a member is core/idl_args' "
                    "idl_global_member_refused, which every §3.7.6 attribute and §3.7.7 operation entry in "
                    "that file calls and which makes the entry return without building anything — so a member "
                    "standing here despite it did not come through one of those entries. Its descriptor may "
                    "well be right, which is why no check above caught it. Route the install through "
                    "core/idl_args' member entries: `git grep '\\\"%s\\\"' engine/host/browser` finds it",
                    name, name, name);
            JS_FreeCString(ctx, name);
            continue;
        }
        DCHECKF(!tab[i].is_enumerable,
                "`%s` is an ENUMERABLE own property of this realm's global, and Web IDL §3.8 Platform objects "
                "implementing interfaces performs DefineMethodProperty(target, id, interfaceObject, false) for "
                "it — ECMAScript §10.2.8 DefineMethodProperty ( homeObj, name, closure, enumerable ) makes that "
                "{ [[Writable]]: true, [[Enumerable]]: FALSE, [[Configurable]]: true }, so a browser never "
                "shows this name to `for (var k in globalThis)` or to `Object.keys(globalThis)`. It got here "
                "through an ordinary [[Set]] rather than through core/idl_args' "
                "idl_define_global_property_reference, which is §3.8's one door. Route the install: "
                "`git grep '\\\"%s\\\"' engine/host/browser` finds it", name, name);
        /* AND THE OTHER HALF OF THAT DOOR. The descriptor above is what a [[Set]] gets wrong; this is what
           EVERY bypass gets wrong, including one whose descriptor is right — §3.3.7 step 1 is asked INSIDE
           idl_define_global_property_reference and nowhere else, so a name that reached this global by any
           other route has never been asked whether it belongs in this realm at all. */
        DCHECKF(idl_exposed_in_realm(ctx, name),
                "`%s` is an own property of this realm's global and Web IDL §3.3.7 [Exposed] step 1 says it "
                "must not be — \"If construct's exposure set is not `*`, and realm.[[GlobalObject]] does not "
                "implement an interface that is in construct's exposure set, then return false\" — so a "
                "browser answers `'%s' in globalThis` FALSE for a global object of this kind. The one place "
                "that step is asked is core/idl_args' idl_define_global_property_reference, which SKIPS the "
                "define when it answers false, so a name standing here despite it did not come through §3.8's "
                "door. Its descriptor may well be right, which is why the check above did not catch it. Route "
                "the install: `git grep '\\\"%s\\\"' engine/host/browser` finds it", name, name, name);
        JS_FreeCString(ctx, name);
    }
    for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    JS_FreeValue(ctx, global);
}

/* ---- Web IDL §3.8's OTHER DIRECTION: every interface object this realm OWES was ASKED for ------------------
   See realm.h for the argument — why §3.7.3's class string is the observation site, why the other side is
   the ASK and not the placement, and why the caller and not this file states that a realm is finished. What
   lives here is the STORE and the walk.

   THE TWO CENSUSES ARE PER-REALM JS OBJECTS IN THE SLOT ARRAY THIS FILE ALREADY OWNS, and that is not a
   convenience: an interface prototype object is built PER REALM, the property reference is asked PER REALM,
   and a module static holding either would be one realm's answer to every realm's question — the defect the
   whole of realm.h exists to prevent, arriving in the auditor rather than in the platform. Being a JS object
   also means each identifier is COPIED into an atom rather than held as a pointer: every identifier reaching
   the notes below is a string literal or a static table entry today, and a census that depended on that would
   be a census with a lifetime rule nobody can see.
   THE OBJECT'S [[Prototype]] IS NULL, so a membership question is a question about what was RECORDED and never
   about %Object.prototype% — `toString` and `constructor` are not interface identifiers, but a set whose
   answers depend on which names ECMAScript happens to define is a set that answers a different question than
   the one asked.
   DEV ONLY, with the walk. */
static JSClassID g_proto_tagged_slot;      /* §3.7.3 — identifiers this realm built an interface prototype object for */
static JSClassID g_reference_asked_slot;   /* §3.8   — identifiers some component asked this realm's global for */

/* The per-realm census object, created on first note. `create` false is a PEEK: a realm that never noted has
   no object, and the walk has to tell that apart from an empty one rather than mint one to read. */
static JSValue realm_census(JSContext *ctx, JSClassID *slot, const char *what, bool create)
{
    JSValue set;

    if (*slot == JS_INVALID_CLASS_ID) {
        if (!create) return JS_NULL;
        *slot = realm_value_declare(ctx, what);
    }
    set = JS_GetClassProto(ctx, *slot);
    if (JS_IsNull(set) && create) {
        set = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(set), "realm: a Web IDL §3.8 census object could not be allocated");
        JS_SetClassProto(ctx, *slot, JS_DupValue(ctx, set));
    }
    return set;   /* OWNED by the caller */
}

/* `value` is CONSUMED. It used to be nothing at all — "this is a set, and the key is the whole of what it
   records" — and that is still true of the §3.8 census, which passes JS_UNDEFINED. It is NOT true of the
   §3.7.3 one: the identifier says an interface prototype object was built and says nothing about WHICH OBJECT,
   and a realm has no other way to reach one by name. A dev-only check that has to read a finished prototype
   therefore had nowhere to get it, which is exactly the gap idl_args.c's [Unscopable] residual named.
   CHECK rather than DCHECK because the define consumes nothing a caller could retry with and the only way it
   fails is allocation — a census that silently dropped an identifier would report a component as having asked
   when it did not, which is the one direction this instrument must never fail in. */
static void realm_census_note(JSContext *ctx, JSClassID *slot, const char *what, const char *name, JSValue value)
{
    JSValue set = realm_census(ctx, slot, what, true);

    CHECK(JS_DefinePropertyValueStr(ctx, set, name, value, JS_PROP_C_W_E) >= 0,
          "realm: an identifier could not be recorded in a Web IDL §3.8 census");
    JS_FreeValue(ctx, set);
}

void realm_note_interface_prototype_object(JSContext *ctx, JSValueConst proto, const char *iface)
{
    DCHECK(iface != NULL && *iface,
           "Web IDL §3.7.3 Interface prototype object's census was told an interface prototype object had been "
           "built for no identifier — §3.7.3 makes the class string the interface's own identifier, so a "
           "prototype with nothing to be called is one nothing downstream can ask a question about");
    DCHECK(JS_IsObject(proto),
           "Web IDL §3.7.3 Interface prototype object's census was handed something that is not an object — "
           "§3.7.3's own step is OrdinaryObjectCreate(proto), so the thing being tagged is an Object or the "
           "algorithm did not run");
    realm_census_note(ctx, &g_proto_tagged_slot,
                      "Web IDL §3.7.3 the interface prototype objects built in this realm", iface,
                      JS_DupValue(ctx, proto));
}

/* THE OBJECT BACK, BY IDENTIFIER — the read the value above exists for, and the one thing a per-realm check
   that must run AFTER a prototype is finished cannot get any other way. An interface this realm never built
   has no row and answers JS_UNDEFINED, which is the SOUND arm and not a failure: a worker realm has no
   Element, and a census of what LANDED must never charge a realm for an interface Web IDL §3.3.7 [Exposed]
   correctly keeps out of it. OWNED by the caller. */
JSValue realm_interface_prototype_object(JSContext *ctx, const char *iface)
{
    JSValue set, proto;

    DCHECK(iface != NULL && *iface,
           "Web IDL §3.7.3 Interface prototype object's census was asked for the object of no identifier");
    set = realm_census(ctx, &g_proto_tagged_slot, NULL, false);
    if (!JS_IsObject(set)) { JS_FreeValue(ctx, set); return JS_UNDEFINED; }
    /* The census object's [[Prototype]] is null, so this read cannot reach %Object.prototype% and answer with
       a function for an interface named `toString` — the same reason the membership walk gives. */
    proto = JS_GetPropertyStr(ctx, set, iface);
    JS_FreeValue(ctx, set);
    /* AND A THROW IS NOT AN ABSENCE. The census object is a plain null-prototype data map, so the only way
       this read fails is allocation — and returning the exception value as though it were "this realm did not
       build that interface" would leave the exception PENDING on the context for whatever runs next, which is
       the one failure this read could cause that has nothing to do with the question asked. */
    CHECK(!JS_IsException(proto),
          "realm: OOM reading the Web IDL §3.7.3 census for an interface prototype object");
    return proto;
}

void realm_note_property_reference_asked(JSContext *ctx, const char *id)
{
    DCHECK(id != NULL && *id,
           "Web IDL §3.8 Platform objects implementing interfaces' census was told a property reference had "
           "been asked for with no identifier — §3.8 keys every one of its DefineMethodProperty calls by an "
           "identifier and there is nothing else to record");
    realm_census_note(ctx, &g_reference_asked_slot,
                      "Web IDL §3.8 the identifiers asked of this realm's global", id, JS_UNDEFINED);
}

void realm_assert_interface_objects_asked(JSContext *ctx)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    JSValue tagged = realm_census(ctx, &g_proto_tagged_slot, NULL, false);
    JSValue asked  = realm_census(ctx, &g_reference_asked_slot, NULL, false);
    int ok;

    /* BOTH POPULATIONS ARE NON-EMPTY, AND THAT IS THE ASSERT THAT KEEPS THE REST FROM BEING VACUOUS. A walk
       over an empty census passes for every realm and every defect, so a recording that silently stopped
       happening — an install path that no longer reaches idl_interface_tag, a note compiled out of one of the
       two doors — would read as a clean audit. This is asked of a FINISHED realm, which by the time it gets
       here has run the whole per-realm intrinsic list, so neither census can legitimately be absent. */
    DCHECK(!JS_IsNull(tagged),
           "a realm was declared FINISHED and Web IDL §3.7.3 built no interface prototype object in it — the "
           "per-realm intrinsic list runs before any caller can say this, and every one of its components "
           "builds at least one, so an empty census is this instrument having stopped recording rather than a "
           "realm having nothing in it. The one recording site is core/idl_args' idl_interface_tag");
    DCHECK(!JS_IsNull(asked),
           "a realm was declared FINISHED and no Web IDL §3.8 property reference was ever asked of its global "
           "— §3.8's door and core/idl_args' idl_install_interface_object_exposed are the two recording sites, "
           "and a realm that reached neither has placed no interface object at all");
    ok = JS_GetOwnPropertyNames(ctx, &tab, &n, tagged, JS_GPN_STRING_MASK);
    DCHECK(ok == 0, "the Web IDL §3.7.3 census of this realm could not be enumerated");
    for (i = 0; i < n; i++) {
        const char *name = JS_AtomToCString(ctx, tab[i].atom);
        const IdlExposureRow *row;
        int has;

        /* A SKIPPED NAME IS A HOLE IN A CENSUS — the same statement the walk above makes, for the same reason:
           the worth of this is that its population is every interface prototype object this realm built. */
        DCHECK(name != NULL,
               "an identifier in this realm's Web IDL §3.7.3 census could not be materialized as a string, so "
               "§3.8's question could not be asked of it — the only way this fails is allocation");
        row = bsearch(name, IDL_EXPOSURE, sizeof IDL_EXPOSURE / sizeof IDL_EXPOSURE[0],
                      sizeof IDL_EXPOSURE[0], idl_exposure_row_name_cmp);
        /* §3.8 PLACES NOTHING FOR THIS IDENTIFIER, so nothing is owed and no bit is needed to say why — see
           realm.h for the two readings this band carries here and why they take the same action. */
        if (row == NULL) { JS_FreeCString(ctx, name); continue; }
        /* AND §3.8's OWN STEP 1 DECIDES WHICH OF THEM THIS REALM IS OWED: "Let interfaces be a list that
           contains every interface that is exposed in realm". A Window-only interface whose prototype a
           per-realm intrinsic built in a WORKER realm is not in that list, so no component owes an ask for it
           — asked of core/idl_args' idl_exposed_in_realm, which is §3.3.7 step 1's one statement in this tree
           rather than a second copy of it written here. */
        if (!idl_exposed_in_realm(ctx, name)) { JS_FreeCString(ctx, name); continue; }
        has = JS_HasProperty(ctx, asked, tab[i].atom);
        CHECK(has >= 0, "a Web IDL §3.8 census probe threw — [[HasProperty]] over a null-prototype object this "
                        "file built runs no page code and has nothing to throw with");
        DCHECKF(has == 1,
                "Web IDL §3.7.3 Interface prototype object built `%s`.prototype in this realm and NOTHING ever "
                "asked §3.8 Platform objects implementing interfaces for the matching `%s` property on its "
                "global — so a page reads `[object %s]` off a live prototype while `%s` itself is undefined and "
                "`x instanceof %s` throws. §3.8's step 1 is \"Let interfaces be a list that contains every "
                "interface that is exposed in realm\", and §3.3.7 [Exposed] step 1 says this realm is one of "
                "them, so the property is owed here. This is NOT the door refusing: the ask is recorded BEFORE "
                "both of §3.3.7's steps, so what is missing is the CALL. The usual cause is an install whose "
                "tail call went with a deleted thunk, leaving the prototype build behind. Route it: "
                "`git grep '\\\"%s\\\"' engine/host/browser` lands on the component that built the prototype, "
                "and the call it owes is core/idl_args' idl_define_global_property_reference",
                name, name, name, name, name, name);
        JS_FreeCString(ctx, name);
    }
    for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    JS_FreeValue(ctx, tagged);
    JS_FreeValue(ctx, asked);
}
#endif

void realm_install_intrinsics(JSContext *ctx, const char *top_level_creation_url,
                              const char *global_interface, bool owner_is_secure_context)
{
    unsigned names;
    int i;

    DCHECK(ctx != NULL, "the per-realm intrinsics were installed into no realm");
    /* AND THE HOST HAS SAID WHETHER IT EXPLORES, BEFORE ANY OF THEM RUN. An intrinsic below may mint a member
       through solver/concolic.h's source seam — navigator.c mints its whole environment record there, once,
       for this realm's LIFETIME — and that seam's answer is the whole of whether the value forks control flow.
       A realm built before the host declares therefore keeps the browser-only answer for the rest of the
       session, with nothing to say so: the members exist, carry the right example, coerce and compare
       correctly, and simply DECIDE at every gate a bundle writes over them instead of forking. Measured on
       this project's own smoke fixture, whose host installed the overlay a hundred lines after bringing its
       agent up: `navigator.userAgent.indexOf('Chrome') >= 0` and `navigator.maxTouchPoints > 0` took one arm
       each for the whole run and the sibling worlds were never created, while `screen.width < 768` in the same
       document forked — because screen.c mints unconditionally and asks nobody.
       IT IS ASSERTED HERE AND NOT AT THE SEAM ALONE because this is the one call every realm of every host
       goes through (the agent's first, and every child navigable's), so a host that gets the order wrong is
       told at the realm it built too early rather than at whichever member happens to be minted first. */
    DCHECK(concolic_source_overlay_declared(),
           "a realm's intrinsics were built before this host declared whether it EXPLORES — a per-realm member "
           "minted through concolic_source_wrap freezes the answer standing at this instant, so every gate a "
           "page writes over one of them would decide instead of forking, for this realm's whole life. Declare "
           "first: concolic_install_source_overlay for a solver host, concolic_declare_browser_only for a "
           "conformance one, both before platform_agent_init");
    /* WHICH FIELDS THIS ENVIRONMENT OWES IS DECIDED BY WHAT KIND OF GLOBAL IT HAS, so the [Global] interface is
       resolved before either of them is checked. That ordering is the fix rather than a detail: this used to
       require a TOP-LEVEL CREATION URL of every realm, on the argument that "§8.1.3.1's field is null only for
       a worker or a worklet, and this engine has neither". The spec half of that sentence is exactly right —
       HTML §10.2.6.2 "Script settings for workers" sets a worker environment's fields as "creation URL to
       worker global scope's url, top-level creation URL to null" — and the half about this tree stopped being
       true, so the assert forbade the one realm kind it had correctly described. */
    names = idl_global_names_of(global_interface);
    if (idl_global_names_are_worklet(names))
        DFAILF("a realm was built stating it implements `%s`, a Web IDL §3.3.8 [Global] WORKLET interface — "
               "HTML §8.1.3.5 Secure contexts step 1.3 (\"If global is a WorkletGlobalScope, then return "
               "true\") is written below and needs no operand, but which of §8.1.3.1 Environments' fields a "
               "WORKLET environment states is not derived anywhere in this tree: §10.2.6.2 Script settings for "
               "workers is a WORKER algorithm and reading it as a worklet's would be a guess about a standard "
               "nobody fetched. Derive the worklet environment's fields and state them here", global_interface);
    if (idl_global_names_are_worker(names)) {
        /* §10.2.6.2 SETS IT TO NULL, so a host that HAS one to pass has built the wrong environment — most
           likely by handing a worker realm the OWNER document's address, which is a Window's field and would
           make §8.1.3.5 answer this realm out of step 2 over a URL that is not this environment's. */
        DCHECK(top_level_creation_url == NULL,
               "a WORKER realm was built WITH a top-level creation URL — HTML §10.2.6.2 Script settings for "
               "workers sets a worker environment's \"creation URL to worker global scope's url, top-level "
               "creation URL to null\", and §8.1.3.5 Secure contexts step 1.2 answers for a WorkerGlobalScope "
               "and returns before step 2 would read one. Pass NULL and the owner's answer");
    } else {
        /* EVERY OTHER ENVIRONMENT HAS ONE, created AT an address. A host with nothing to pass here has not
           decided which document this realm is, which is the same thing location.c's install asserts one layer
           up and for the same reason: the empty string used to be answered quietly and the quiet answer hid a
           host passing the wrong field entirely. */
        DCHECK(top_level_creation_url != NULL && *top_level_creation_url,
               "a realm was built with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide whether "
               "this environment is a SECURE CONTEXT, and Web IDL §3.3.13's members are installed or absent by "
               "that answer, so a realm without it is a realm whose platform surface is undecided");
        /* AND IT HAS NO OWNER SET TO HAVE AN ANSWER FROM. §8.1.3.5 step 1.2.1 reads "global's owner set[0]'s
           relevant settings object", and an owner set is §10.2.1.1 state that only a WorkerGlobalScope has —
           so `true` here is a host stating a fact that does not exist, and `false` is the one value that says
           nothing. Asserting it is what makes the fourth argument impossible to state wrongly rather than
           merely easy to state correctly. */
        DCHECK(!owner_is_secure_context,
               "a non-worker realm was built stating an OWNER's secure-context answer — HTML §8.1.3.5 Secure "
               "contexts step 1.2.1 reads that from a WorkerGlobalScope's §10.2.1.1 owner set, and a Window "
               "environment has no owner set for the value to be about. A Window realm passes false");
    }
    /* AND WHICH [Global] INTERFACE ITS GLOBAL OBJECT IMPLEMENTS, for the same reason and one line further on:
       Web IDL §3.3.7 [Exposed] step 1 decides which interface objects the intrinsics below may put on this
       global at all, so the answer has to exist before the first of them runs. A host that does not state it
       has not decided what KIND of realm this is, which is a different question from which document it is —
       and one this engine could not previously ask, which is why every realm it builds is a Window one. */
    DCHECK(global_interface != NULL && *global_interface,
           "a realm was built with no [Global] INTERFACE — Web IDL §3.3.7 [Exposed] step 1 asks which "
           "interface this realm's global object implements, and the answer decides which of the platform's "
           "interface objects exist here at all. A Window realm states \"Window\"");
    if (g_top_level_url_slot == JS_INVALID_CLASS_ID)
        g_top_level_url_slot = realm_value_declare(ctx, "HTML §8.1.3.1 the environment's top-level creation URL");
    if (g_global_names_slot == JS_INVALID_CLASS_ID)
        g_global_names_slot = realm_value_declare(ctx, "Web IDL §3.3.8 [Global] the realm's global names");
    if (g_owner_secure_slot == JS_INVALID_CLASS_ID)
        g_owner_secure_slot = realm_value_declare(ctx, "HTML §8.1.3.5 the worker owner's secure-context answer");
    /* THE GLOBAL NAMES FIRST, because everything below and every install after it reads them: §3.3.7 step 1
       through idl_exposed_in_realm, and §8.1.3.5's own branch through realm_global_is_worker. */
    realm_value_set(ctx, g_global_names_slot, JS_NewInt32(ctx, (int32_t)names));
    if (idl_global_names_are_worker(names)) {
        realm_value_set(ctx, g_owner_secure_slot, JS_NewBool(ctx, owner_is_secure_context));
    } else {
        JSValue url = JS_NewString(ctx, top_level_creation_url);

        CHECK(!JS_IsException(url), "realm: the environment's top-level creation URL could not be allocated");
        realm_value_set(ctx, g_top_level_url_slot, url);
    }
    /* THE LIST IS CLOSED FROM HERE. Set before the walk rather than after it, because an install that reached
       realm_declare_intrinsic would be declaring into the very vector this loop is indexing. */
    g_realm_built = true;
    for (i = 0; i < g_n; i++)
        g_list[i](ctx);
#if APICLIENT_DEV
    /* EVERY §3.8 PROPERTY REFERENCE THE PER-REALM INTRINSIC LIST PLACED. It is asked here and not inside a
       component because the invariant is about the GLOBAL and not about one name: a component can only see the
       one it installed, and what has to be true is a statement about the whole object.
       IT IS NOT NECESSARILY THE WHOLE OF THIS REALM'S GLOBAL, and saying so is the point — for a realm whose
       global object is a WorkerGlobalScope this IS the end of its construction, and for a WINDOW realm
       core/platform.c's per-document install column runs after this returns and asks the same question again
       at the end that finishes THAT realm. See the banner above for why one function with two callers is
       routing rather than a second copy. */
    realm_assert_global_property_references(ctx);

    /* AND WEB IDL §3.7.3's [Unscopable] BLOCK AGAINST THE MEMBERS THIS LIST JUST INSTALLED. It belongs HERE and
       not at the owed-half entry below, and the difference is a population rather than a preference: that entry
       states §3.8's condition and is not reached by a realm that never installs a document, while this one
       needs §3.7.6's and §3.7.7's — every member is on its prototype — which is true of every realm exactly at
       the end of this loop, because an interface that carries an [Unscopable] row declares its install here and
       defines its members in the same function that tags its prototype. core/idl_args.c owns the question; what
       this line states is the ORDERING fact, which is this file's to state and not that one's. */
    idl_assert_unscopables_name_members(ctx);
#endif
}

JSValue realm_top_level_creation_url(JSContext *ctx)
{
    DCHECK(g_top_level_url_slot != JS_INVALID_CLASS_ID,
           "a realm's top-level creation URL was read in an agent where no realm has been built — the field is "
           "created WITH the realm, so a reader that gets here is standing outside every realm there is");
    /* AND THIS REALM'S ENVIRONMENT HAS ONE. HTML §10.2.6.2 Script settings for workers sets a worker
       environment's "top-level creation URL to null", so the slot is deliberately not written for such a realm
       — and it is asserted HERE rather than answered with a stand-in because every reader of this field acts
       on the address: §8.1.3.5 decides a platform surface from it, and the other two INHERIT it into a child
       navigable. A stand-in would be a plausible datum in all three. */
    DCHECK(!realm_global_is_worker(ctx),
           "HTML §8.1.3.1 Environments' top-level creation URL was read in a WORKER realm, where §10.2.6.2 "
           "Script settings for workers sets it to null. §8.1.3.5 Secure contexts asks this realm's question "
           "at its step 1.2 instead (realm_owner_is_secure_context); a reader inheriting the field into a "
           "child navigable is standing in a realm that has none, because a worker has no navigables");
    return realm_value_get(ctx, g_top_level_url_slot);   /* asserts THIS realm ran the install */
}

bool realm_global_is_worker(JSContext *ctx)
{
    return idl_global_names_are_worker(realm_global_names(ctx));
}

bool realm_global_is_worklet(JSContext *ctx)
{
    return idl_global_names_are_worklet(realm_global_names(ctx));
}

bool realm_owner_is_secure_context(JSContext *ctx)
{
    JSValue v;
    bool r;

    DCHECK(g_owner_secure_slot != JS_INVALID_CLASS_ID,
           "HTML §8.1.3.5 step 1.2.1's operand was read in an agent where no realm has been built — the field "
           "is created WITH the realm, so a reader that gets here is standing outside every realm there is");
    /* A WORKER REALM IS THE ONLY ONE THAT WRITES IT, so this is the same two-sided statement the URL accessor
       above makes from the other side: neither field is answered for the realm kind that does not state it. */
    DCHECK(realm_global_is_worker(ctx),
           "HTML §8.1.3.5 Secure contexts step 1.2.1's operand — the worker owner's own secure-context answer "
           "— was read in a realm whose global is not a WorkerGlobalScope. Step 1.2 is what reaches 1.2.1, and "
           "a realm that does not take that branch is answered by step 2 over its top-level creation URL");
    v = realm_value_get(ctx, g_owner_secure_slot);   /* asserts THIS realm ran the install */
    /* READ OFF THE TAG, NEVER THROUGH A COERCION — the same rule realm_global_names states just below, and for
       the identical reason: JS_ToBool's result is what would say whether the read worked, so a check that read
       it would be a check with a side effect, compiled out in release. This value is one this file wrote with
       JS_NewBool and nothing else can reach the slot. */
    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_BOOL,
           "HTML §8.1.3.5 step 1.2.1's operand is not the boolean this file wrote there — the slot is written "
           "once, by realm_install_intrinsics, from the answer the creating agent stated");
    r = JS_VALUE_GET_BOOL(v) != 0;
    JS_FreeValue(ctx, v);
    return r;
}

unsigned realm_global_names(JSContext *ctx)
{
    JSValue v;
    int32_t names = 0;

    DCHECK(g_global_names_slot != JS_INVALID_CLASS_ID,
           "a realm's Web IDL §3.3.8 [Global] global names were read in an agent where no realm has been "
           "built — the field is created WITH the realm, so a reader that gets here is standing outside every "
           "realm there is");
    v = realm_value_get(ctx, g_global_names_slot);   /* asserts THIS realm ran the install */
    /* READ OFF THE TAG, NEVER THROUGH A COERCION. `JS_ToInt32` would have been the obvious spelling and it is
       the banned one: its result is what says whether the read worked, so the check that reads it is a check
       with a SIDE EFFECT — compiled out in release, `names` is never written, every construct whose exposure
       set is not `*` loses its global property, and a release build's whole platform surface goes with it.
       The value is one this file wrote with JS_NewInt32 and nothing else can reach the slot, so the tag is the
       invariant and reading it is not a conversion at all. */
    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
           "a realm's Web IDL §3.3.8 [Global] global names are not the integer this file wrote there — the "
           "slot is written once, by realm_install_intrinsics, with the mask idl_global_names_of resolved");
    names = JS_VALUE_GET_INT(v);
    JS_FreeValue(ctx, v);
    return (unsigned)names;
}

void realm_intrinsics_free(void)
{
    free(g_list);
    g_list = NULL;
    g_n = g_cap = 0;
    /* AND THE AGENT MAY BE BROUGHT UP AGAIN. This is the teardown of an agent's whole declaration state, so
       the closure it recorded goes with the list it was about — a stale `true` here would refuse the next
       agent's entire declare column. */
    g_realm_built = false;
    /* The slot's VALUES are the realms' and went with them; what the agent holds is the slot id, which is a
       class id in a runtime that is going away with it. */
    g_top_level_url_slot = JS_INVALID_CLASS_ID;
    g_global_names_slot = JS_INVALID_CLASS_ID;
    g_owner_secure_slot = JS_INVALID_CLASS_ID;
#if APICLIENT_DEV
    /* The two §3.8 censuses are slots like the three above and are released the same way — their CONTENTS are
       the realms' and went with them; what the agent holds is the slot id. */
    g_proto_tagged_slot = JS_INVALID_CLASS_ID;
    g_reference_asked_slot = JS_INVALID_CLASS_ID;
#endif
}

/* A slot IS a class id whose per-context prototype slot holds something that is not a prototype. Nothing is
   ever constructed with the class — it exists for the SLOT, which is the per-realm store quickjs already keeps
   and already frees with the context. The `what` string is the class name, so a heap dump names the slot.
   AND THE ENGINE IS TOLD THAT THIS ENTRY IS A SLOT, WHICH USED TO BE A PLAIN `JS_NewClass` AND IS THE ROOT OF
   A CRASH ON EVERY REAL APPLICATION PAGE. Registering it as an ordinary class put a free-form DESCRIPTION into
   a field whose readers take it for a class BRAND, and quickjs's intrinsic namer is one of those readers: it
   walks `ctx->class_proto` for the object it was asked about, finds it in a slot, and composes
   `%<that description>.prototype%` as the object's CONSTRAINT KEY. Every part of that is wrong at once — the
   object is not a prototype, the description is not a class name, and a description carrying a `.` collides
   with the very `X.prototype` spelling the namer's separator is. Measured: 89 of the 97 names declared through
   this door carry a dot, and `CSSOM §6.6.1 CSSStyleDeclaration.prototype` aborted a real page 2 of 2 drives.
   THE FIX IS THE KIND AND NEVER THE SPELLING. Shortening these descriptions to dotless tokens would repair the
   collision and leave the wrong claim standing — the namer would go on reporting a per-realm map as some
   class's prototype — and it would be a sweep over prose that is deliberate and argued for at each site. So
   the slot says what it IS, once, at the one door that mints one, and the namer passes it over; §NO BOUNDS is
   untouched because nothing was hashed, escaped or truncated, and no name became ambiguous. */
JSClassID realm_value_declare(JSContext *ctx, const char *what)
{
    JSClassID id = 0;

    DCHECK(what != NULL && *what, "a per-realm value was declared with no name — a dump would not say whose");
    JS_NewClassID(JS_GetRuntime(ctx), &id);
    CHECK(JS_NewRealmValueSlotClass(JS_GetRuntime(ctx), id, what) == 0,
          "realm: a per-realm value slot could not be declared");
    return id;
}

/* THE SITE IS THE CALLER'S AND IT IS NOT OPTIONAL — see realm.h's macros, which supply it. A NULL is the one
   way a site could still be absent once both entries take the pair, and it is refused rather than printed as
   "(null)": a record whose address reads `(null):0` is a plausible datum in exactly the field this mechanism
   exists to make load-bearing.
   IT IS A MACRO AND NOT A HELPER, for the same reason the entries themselves are macros: a shared function
   would stamp its own line for both entries, so the one assert in this file whose whole subject is a MISSING
   address would itself name neither the read nor the write. Expanded here it names which. */
#define REALM_VALUE_SITE_PRESENT(at_file_) \
    DCHECK((at_file_) != NULL, \
           "a per-realm value was reached with no call site — realm.h's realm_value_get/realm_value_set " \
           "macros are what supply the pair at the call, and a realm.c-internal path with no caller to name " \
           "passes REALM_VALUE_SITE_INTERNAL rather than a null")

/* THE SLOT IS A MINTED CLASS ID, AND THAT IS WHAT IS ASSERTED — NOT THAT IT SITS OUTSIDE A LIST OF SENTINELS.
   Both entries USED TO READ `slot > 0`, which is a RANGE over the two pre-declaration values an `int` can
   hold — a property of the slot's C TYPE and not of the quantity. That is the wrong shape of predicate twice
   over, and the reasoning is kept because a reader who re-derives it will write the range back.
     - IT DOES NOT SURVIVE THE TYPE THE QUANTITY ACTUALLY HAS. A slot is a class id and quickjs spells one
       `typedef uint32_t JSClassID`, under which `> 0` refuses `0` and ADMITS `-1`: it arrives as 0xFFFFFFFF,
       passes, and indexes `ctx->class_proto` out of bounds — a wild read in release, where every assert
       between this line and the array is compiled out. A guard whose correctness depends on the operand
       staying signed is a guard that a signature change silently turns into a hole.
     - AND IT IS WEAKER THAN THE INVARIANT EVEN WHERE IT HOLDS. `int` is also the C type of a step id and of
       core/idl_args' method id in this engine, and every one of those is `> 0` too. The range admits an
       operand that was never a slot at all, which is exactly the confusion a range cannot see.
   WHAT REPLACES IT IS THE RUNTIME'S OWN QUESTION, asked of the runtime rather than of the bit pattern.
   JS_IsRegisteredClass is `class_id < rt->class_count && rt->class_array[class_id].class_id != 0` over a
   `JSClassID` parameter, and each conjunct earns its place here: the first is a BOUNDS PROOF for the very
   array both entries are about to index, since JS_NewClass1 grows `class_proto` and `class_count` together;
   the second refuses JS_INVALID_CLASS_ID, which quickjs defines as 0 and which nothing can register because
   JS_CLASS_OBJECT is 1 and `JS_NewClassID` treats a zero as not-yet-minted. `-1` fails the first conjunct
   under either signedness, so this one predicate was correct while the operand was still `int` and is correct
   now that it is `JSClassID` — which is why it could land BEFORE the type did. It also refuses a
   registered-looking integer that no mint ever handed out, which the range it replaced admitted.
   IT IS A MACRO FOR THE REASON `REALM_VALUE_SITE_PRESENT` IS: a shared function would stamp its own line for
   both entries, so a @WHY about a slot would name neither the read nor the write. The verb is the parameter
   because the caller's address says WHERE and only the entry says WHICH DOOR the operand came through. */
#define REALM_VALUE_SLOT_MINTED(ctx_, slot_, verb_, at_file_, at_line_) \
    DCHECKF(JS_IsRegisteredClass(JS_GetRuntime(ctx_), (slot_)), \
            "%s:%d " verb_ " a per-realm value through a slot this runtime never minted — the slot is the " \
            "class id realm_value_declare returns, so a component whose own `_init` never ran, or whose " \
            "static slot was read before that `_init` wrote it, arrives here holding its pre-declaration " \
            "value rather than a class id", \
            (at_file_), (at_line_))

void realm_value_set_at(JSContext *ctx, JSClassID slot, JSValue v, const char *at_file, int at_line)
{
    JSValue prev;

    REALM_VALUE_SITE_PRESENT(at_file);
    REALM_VALUE_SLOT_MINTED(ctx, slot, "set", at_file, at_line);
    prev = JS_GetClassProto(ctx, slot);
    DCHECKF(JS_IsNull(prev), "%s:%d set a per-realm value twice in one realm — the first is what everything "
                             "already built in this realm is holding, so this write would hand later readers "
                             "a different object than the one earlier readers are already using",
            at_file, at_line);
    JS_FreeValue(ctx, prev);
    JS_SetClassProto(ctx, slot, v);
}

JSValue realm_value_get_at(JSContext *ctx, JSClassID slot, const char *at_file, int at_line)
{
    JSValue v;

    REALM_VALUE_SITE_PRESENT(at_file);
    REALM_VALUE_SLOT_MINTED(ctx, slot, "read", at_file, at_line);
    v = JS_GetClassProto(ctx, slot);
    /* THE ADDRESS IS THE WHOLE OF THE REMEDY HERE. What went wrong is a missing SET, and the set is in some
       other component's per-realm install — so the reader's own file and line is what says which install to
       look for, and which realm kind (a worker's, a worklet's, a child navigable's) reached a component that
       only ever installs into one of them. */
    DCHECKF(!JS_IsNull(v), "%s:%d read a per-realm value in a realm that never ran the install that sets it — "
                           "this read's own component is what names the slot, and the install that writes it "
                           "is declared through realm_declare_intrinsic and run for every realm, so a realm "
                           "that reached here without it is one whose builder does not run that component",
            at_file, at_line);
    return v;
}

void realm_awaits(JSContext *ctx, const char *path, const char *what)
{
    JSValue cur = JS_GetGlobalObject(ctx);
    const char *p = path;
    bool present = false;

    DCHECK(path != NULL && *path, "a producer assertion named no producer");
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        char name[64];
        JSAtom atom;
        int has;

        DCHECK(n > 0 && n < sizeof(name), "a producer assertion named a path this cannot read");
        if (!JS_IsObject(cur)) break;            /* the path died on the way; the producer is absent */
        memcpy(name, p, n);
        name[n] = 0;

        atom = JS_NewAtomLen(ctx, name, n);
        has = JS_HasProperty(ctx, cur, atom);
        CHECK(has >= 0, "a producer probe threw — [[HasProperty]] over an engine global runs no page code and "
                        "has nothing to throw with");
        if (!has) { JS_FreeAtom(ctx, atom); break; }
        if (!dot) { JS_FreeAtom(ctx, atom); present = true; break; }   /* the member exists: producer is built */

        {
            JSValue next = JS_GetProperty(ctx, cur, atom);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, cur);
            cur = next;
        }
        p = dot + 1;
    }
    JS_FreeValue(ctx, cur);
    DCHECK(!present, what);
}
