/* WHAT A RELEASE OWES, STATED ONCE AND CHECKED AT THE RELEASE.
 *
 * core/platform.h's third column ended the drift between three hosts' teardown LISTS. It said nothing about
 * what any one release DOES, and the twenty-eight releases on that column had answered that independently:
 *
 *   - fetch_free gave back §5's four interned names and left g_fetch_stepid, g_deliver_stepid and g_fetch_rt
 *     exactly as fetch_init had set them, while fetch_init opens with `if (g_fetch_stepid >= 0) return;`. A
 *     SECOND agent in one process therefore got a fetch that reported itself declared and whose four atom
 *     handles were all JS_ATOM_NULL — and JS_ATOM_NULL is a VALID atom id (it is `<null>`), so every one of
 *     those reads answers, wrongly, with no crash anywhere.
 *   - dom_rect_free and dom_rect_list_free left their class ids set, and both inits open on the same test.
 *   - idb_transaction_init registered §2.7.1's cleanup with the ONE frontier (engine_set_checkpoint_hook) and
 *     its release cleared nothing, so the solver held a callback into a component whose two live sets had just
 *     been freed — and solver_agent_free runs AFTER platform_agent_free.
 *   - window_message_init had no double-declaration assert at all, so it was the one component whose init
 *     would have leaked its own delivery callee rather than crash.
 *
 * NONE OF THOSE IS VISIBLE TO ANY DETECTOR THIS TREE HAS. JS_FreeRuntime's gc_obj_list walk and its atom
 * census report what was not GIVEN BACK; a stale handle gave its reference back and then kept the number. The
 * only reader of that number is the component's own next `_init`, and by the time it reads one the agent that
 * wrote it is gone.
 *
 * SO THE PRE-INIT STATE IS DECLARED WHERE IT IS CREATED, AND ASSERTED WHERE IT IS UNDONE. A component names,
 * beside the line that sets it, each static that holds agent-lifetime state — the latch its own init consults
 * first among them — and platform_agent_free asserts every one of them is back at the value a fresh process
 * would have found there. A release that frees a value and forgets the handle now CRASHES at the release,
 * naming the component and the slot, instead of handing out a torn-down world an agent later.
 *
 * THE KIND IS THE PRE-INIT VALUE. `-1` for an id (a pool entry, a step definition) and for a per-realm value
 * slot, `0` for a class id or a flag, JS_ATOM_NULL for an interned name, JS_UNDEFINED for a JSValue, NULL for
 * a pointer — the same values C gives a static before anything runs, except where the component's own
 * initialiser states otherwise. There is one function per kind rather than one function and an enum, so the
 * COMPILER checks that the slot really is what its declaration says it is — EXCEPT BETWEEN AN ID AND A REALM
 * SLOT, which are both `int` and which no signature can therefore separate. That one pair is the reason
 * agent_state_realm_slot exists at all and is argued at its own entry below.
 *
 * AND A CLASS ID IS GIVEN BACK LIKE EVERY OTHER SLOT — ONE POLICY, WRITTEN HERE BECAUSE TWO SELF-CONSISTENT
 * ONES ARE WORSE THAN EITHER OF THEM. Components had been settling this per file and had settled it BOTH WAYS:
 * some put the id back at 0 at their release, some left it set and let the `if (g_class) return;` latch their
 * init opens with read it again. Each is coherent read alone, which is exactly why the mixture went unnoticed.
 * It is not coherent together: JS_NewClassID draws from `rt->js_class_id_alloc`, a second agent's runtime
 * restarts that counter at JS_CLASS_INIT_COUNT, so a zeroing component draws an id a carrying component is
 * about to hand JS_NewClass1 — which refuses an id already taken.
 * THE COLLISION IS NOT THE ARGUMENT, THOUGH, and that is what makes this decidable rather than a preference: a
 * class is registered in a RUNTIME. A carried id names a class in a runtime that is gone, and because the id
 * doubles as the latch, the next agent's `_init` RETURNS BEFORE RE-REGISTERING IT — so every object that
 * component mints is branded with an id the live runtime never gave out, and every `JS_GetOpaque(v, id)` in it
 * answers about whichever class did get that number. Carrying is not the cautious half of a tie; it is wrong on
 * its own, in one agent's successor, with no second component required. So `0` in the table above is the whole
 * of the rule, and what it costs is the paragraph at the end of this comment: a finalizer and a gc_mark run
 * AFTER the release column, so they reach the record with JS_GetAnyOpaque — never by looking up an id their own
 * release has already given back.
 *
 * IT IS TWO-SIDED FROM core/platform.c, WHICH IS WHERE THE FORCING LIVES. A row with a release that declares
 * no agent state is a release column nothing can check, and a row with NO release that declares agent state is
 * a component holding what nobody frees — the exact shape of every leak that file's comments record. Both are
 * asserted the moment the declaration pass ends, so neither can be reached by adding a component.
 *
 * AND THERE IS A THIRD DIRECTION, WHICH NEITHER OF THOSE CAN ASK, BECAUSE BOTH ARE QUESTIONS ABOUT A ROW.
 * `component` here and `name` on that row are TWO INDEPENDENTLY WRITTEN SPELLINGS of one thing, so a
 * declaration can name a row that does not exist — and that is not a weaker pairing, it is an ABSENT one with
 * a LIE beside it. The slots go unpaired, so the arm the pairing exists for (does anybody RELEASE this?) is
 * never asked about them at all; and the row that really owns them then answers "declared no agent state" in
 * character-for-character the words a component that truly declared nothing produces. THREE STATES BEHIND ONE
 * ANSWER, which is the defect this whole file was written against, arriving through the NAME instead of
 * through a value. So the registry is also walked the other way — every declaration must name a row — and it
 * is walked FIRST, so that the two row-directions can state in their own messages that a misspelling has
 * already been ruled out.
 *
 * A SUB-COMPONENT NAMES THE ROW THAT RELEASES IT, NOT ITS OWN FILE, and that is why this cannot be a macro
 * emitting both halves at the declaration. core/platform.c's list is not a list of FILES: a component reached
 * only through another one's init and given back only by that one's release (Selection and currentScript under
 * `document`, sendBeacon under `navigator`, SubtleCrypto under `crypto`, IntersectionObserverEntry under
 * `intersection_observer`) has no row of its own and must not be given one, because a row is precisely a
 * DECLARE and a RELEASE that platform.c itself calls. The name written here is therefore a CLAIM about which
 * release undoes this — "document_agent_free reaches selection_free" — and no spelling scheme can check a
 * claim of that shape. What checks it is agent_state_check_released below, at the one instant it is decidable:
 * a name that is merely SPELLED right and belongs to a release that does not reach this slot fires there.
 * THAT LAST SENTENCE WAS NARROWED ONCE AND THE NARROWING IS CLOSED RATHER THAN RESTATED, and the retired
 * reasoning is kept because a reader who re-derives the undo's reach re-derives the hole with it. It read
 * `the misattributed slots are put back by a release that never reached them and the walk passes` — true of
 * a row that ends in agent_state_undo and not of one that hand-resets, because the undo's unit is the ROW and
 * a row is declared from SEVERAL FILES whose releases the cascade reaches as a TREE. The REACH was never the
 * defect; what was missing was a PRECONDITION on it. agent_state_reached below is how a declaring file says
 * the cascade reached it, and the undo now ABORTS rather than put back a slot declared in a file that has not
 * said so. The sentence therefore holds for both kinds of row, by two mechanisms — a hand-resetting row
 * leaves the slot SET for the walk to find, a row ending in the undo REFUSES to clear it — so the direction
 * no longer gets quieter as this file's own advice about the undo is taken.
 *
 * AND WHAT THIS COSTS: A FINALIZER AND A gc_mark RUN AFTER THE RELEASE COLUMN, SO NEITHER MAY READ A SLOT
 * DECLARED HERE. This is the obligation the zeroing above creates, and it is stated here because here is where
 * it is created. The collection that finalizes the PAGE'S object graph is not the release: a document's
 * objects are held by their realm, the realm is released with the runtime, and platform_agent_free runs before
 * both — so every host's teardown is `platform_agent_free()` … `JS_RunGC` … `JS_FreeRuntime` in that order, and
 * this fork's JS_FreeRuntime has no sweep of survivors at all (it collects, then ASSERTS `gc_obj_list` empty).
 * A component's finalizer therefore runs with its own class id already back at 0, and `JS_GetOpaque(val, 0)`
 * answers NULL for every object of it. FOUR components were reached that way and each failed differently:
 *   - core/geometry/dom_rect.c leaked the box and its four owned values for any page holding a
 *     `getBoundingClientRect()` result — and its gc_mark was worse than the finalizer, because an unmarked
 *     child keeps the internal reference gc_decref subtracts, so gc_scan reads it as rooted from OUTSIDE the
 *     heap and it is never collected at all. Silent.
 *   - core/file/file_system_handle.c leaked an FsLocator, its path array and its root string per live handle —
 *     silent in dev AND release, because a malloc'd block appears in neither of JS_FreeRuntime's censuses.
 *   - core/frame/remote_object.c already read the opaque correctly and then DCHECKed the live object's class
 *     against the two ids its release had zeroed: a guaranteed FALSE `@WHY` for any live reference. THAT is
 *     why the rule is "reads NO static its own release resets" and not "use JS_GetAnyOpaque".
 *   - core/events/message_port.c stacked three, each masking the next, and aborted on any React page.
 * SO: reach the record with JS_GetAnyOpaque — the collector dispatched to that function THROUGH the class, so
 * the id is a fact it already has and must not look up — and give anything else the finalizer touches a
 * lifetime that outlives the column (message_port.c's live-port table is released by the LAST port; the
 * remote-navigable rows in core/frame/window_proxy.c are emptied so the later scan finds nothing). A slot whose
 * value is legitimately non-pre-init at the release is NOT agent state in this header's sense and must not be
 * declared: assert its pre-init value at the next `_init`, which is the moment it is true. */
#ifndef ENGINE_HOST_BROWSER_CORE_AGENT_STATE_H
#define ENGINE_HOST_BROWSER_CORE_AGENT_STATE_H

#include <stdbool.h>

#include "quickjs.h"

/* DECLARED ONCE PER AGENT, by the component, beside the line of its own `_init` that sets the slot.
   `component` is A ROW OF core/platform.c's LIST — the row whose RELEASE gives this slot back, which for a
   sub-component is the row that reaches it and never its own file's name. `what` is what the slot IS — it is
   the second half of the assert a forgotten release fires, so it names the state and not the variable, and
   for a sub-component it names its own STANDARD too, since it is read out of a report headed by the row. */
/* AND THE DECLARING SITE TRAVELS WITH THE DECLARATION, WHICH IS WHY EACH OF THESE IS A MACRO OVER AN `_at`
 * FUNCTION RATHER THAN A FUNCTION. Every assert this registry can fire — the duplicate below, the pre-init
 * check at the release, and core/platform.c's walk over declarations that name no row — is written at ONE
 * line, and a DCHECK stamps the file and line it is WRITTEN at. There are 611 declaring call sites reaching
 * those lines, so the crash named a COMPONENT and a STATE and no address; CLAUDE.md's rule for exactly this
 * shape and exactly this scale is that the site is part of the assert rather than decoration on it.
 *
 * WHAT IT COSTS TO READ ONE WITHOUT IT, MEASURED RATHER THAN ARGUED. Three aborts in one session came out of
 * that walk. For `canvas_rendering_context_2d` the name happened to be the file's, so the address was
 * recoverable by reading it; for `html_canvas_element` the repair was to name SOMEBODY ELSE'S row, so the
 * name was about to stop being the file's and the report would have named neither the file to edit nor the
 * row to pick. The two states that walk separates take opposite repairs, and choosing between them is a
 * question about the DECLARING FILE — is this a component core/platform.c owes a row, or a sub-component
 * whose owner releases it — which is the one fact a message built from `component` and `what` cannot carry.
 *
 * WHY A MACRO AND NOT A HELPER, and why no call site changed. __FILE__ and __LINE__ inside a function are
 * THAT function's, so a forwarding hop would stamp this file for all 611 of them, which is the defect rather
 * than the cure. A function-like macro is expanded AT THE CALL, so the pair is the caller's by construction
 * and the existing calls are unchanged text — including the ones in components this lane may not edit,
 * which is what makes the address affordable at all. This is core/idl_args.h's IDL_SITE convention, routed
 * to rather than re-derived; the one difference is that there are no callers here that spell the pair
 * themselves, so there is no `_INTERNAL` sentinel to declare and no forwarder that could want one.
 *
 * THE PAIR IS REQUIRED AND NOT DEFAULTED: every `_at` entry takes both, so a caller reaching one without a
 * site does not compile, and the file pointer is DCHECKed rather than tolerated. A defaulted address is what
 * lets a caller with nothing to say look like one that was never converted. */
void agent_state_id_at(const char *component, const int *slot, const char *what,
                       const char *file, int line);                                   /* pre-init: -1 */
void agent_state_flag_at(const char *component, const int *slot, const char *what,
                         const char *file, int line);                                 /* pre-init: 0 */
void agent_state_class_at(const char *component, const JSClassID *slot, const char *what,
                          const char *file, int line);                                /* pre-init: 0 */
void agent_state_atom_at(const char *component, const JSAtom *slot, const char *what,
                         const char *file, int line);                                 /* pre-init: JS_ATOM_NULL */
void agent_state_value_at(const char *component, const JSValue *slot, const char *what,
                          const char *file, int line);                                /* pre-init: JS_UNDEFINED */
/* A POINTER SLOT — a recorded JSRuntime, a malloc'd buffer, a hook this component installed into another. The
   address crosses as `const void *` and the check compares the BYTES of a null pointer, because reading a
   `JSRuntime *` object through a `void *` lvalue is the strict-aliasing violation CLAUDE.md's §C-stack rule
   was written about; memcmp reads unsigned chars and is legal for every object there is. */
void agent_state_ptr_at(const char *component, const void *slot, const char *what,
                        const char *file, int line);
/* A PER-REALM VALUE SLOT — core/realm.h's realm_value_declare handed this out, and it is A CLASS ID.
 *
 * WHY IT IS NOT AN id, WHICH IS WHAT IT WAS. `int` is the C type of three unrelated quantities in this
 * engine — a step id from JS_RegisterStepDef, a method id from core/idl_args' idl_method_id (which IS a step
 * id: that function's last line returns one), and a per-realm value slot — and while all three went through
 * agent_state_id the registry could not tell them apart. That is not untidiness. A slot and a class id are
 * THE SAME OBJECT: realm_value_declare's body is JS_NewClassID plus JS_NewClass over a local that starts at
 * 0, so it always mints, and what it returns is `rt->js_class_id_alloc` at the moment of the call. So the
 * registry holds, between SLOT_CLASS and this kind, every class id anybody told it about — and `js_class_id_alloc`
 * less JS_CLASS_INIT_COUNT is exactly how many this agent minted. Those two numbers are a CONSERVATION
 * IDENTITY over ONE allocator, which no spelling of a mint can evade, and it is what turns an undeclared
 * class id from something a text sweep COUNTS into something that cannot be constructed. It could not be
 * written while a realm slot and a step id were one kind, because the left-hand side was not derivable.
 * RETIREMENT: this paragraph goes when the identity below is asserted, because the reason for the kind is
 * then re-derivable from the assert instead of from here.
 *
 * WHY THE PRE-INIT IS `-1` AND NOT `0`, WHICH IS THE ANSWER THE ARGUMENT ABOVE INVITES AND WHICH THIS TREE
 * REFUTES. A class id's never-minted value is 0; core/realm.c says so of its own slots in as many words
 * (`Zero is the 'not declared' value because it is also the invalid slot realm_value_set asserts against`);
 * and both accessors refuse `slot > 0`. Every step of that is true and it does not reach this entry, for two
 * measured reasons and one read one.
 *   - THE ACCESSORS DO NOT PICK. realm_value_set_at and realm_value_get_at assert `slot > 0`, which is a
 *     RANGE: `0` and `-1` are both absent to them, so neither is the sentinel the read side asks for.
 *   - THE COMPONENTS HAVE PICKED, AND THEY PICKED `-1`. Derived at 8de85780, over every realm slot in
 *     engine/host: 61 of 75 initialise their own static to `-1` and 28 of those gate their re-declaration on
 *     its SIGN (`< 0` / `>= 0`); the 14 that rely on C's implicit `0` gate on ZERO where they gate at all.
 *     Those are two self-consistent conventions, which is the same mixture the class-id paragraph at the top
 *     of this file condemns — and the split is not even: every one of the 43 slots this registry was already
 *     told about is a `-1` slot, and 21 of those gate on the sign. A kind whose pre-init were `0` would put a
 *     `-1`-gated init back at a value
 *     its own latch reads as DECLARED, so the next agent would skip the mint and every realm_value_set in it
 *     would abort; and it would make agent_state_check_released fire on the 24 rows that hand-reset to `-1`,
 *     which are correct today. So `-1` is not the cautious half of a tie here: it is what the declared
 *     population IS, and `0` is a sweep across those components rather than a property of this entry.
 *     Re-derive rather than believing the figures: `node engine/agentstate.mjs --rev <rev>` bands them.
 *   - AND `-1` IS WORSE IN EXACTLY ONE PLACE, WHICH IS WHY THAT SWEEP IS WORTH SOMEBODY'S DIFF AND IS NOT
 *     THIS ONE. The closing paragraph of this file already forbids a finalizer or a gc_mark to read a slot
 *     its own release has reset. Where one does it anyway, the two sentinels are not alike in a RELEASE
 *     build, where every assert between the read and the array is compiled out: realm_value_get_at casts the
 *     slot to JSClassID and indexes `ctx->class_proto`, so `0` reads element zero — in bounds, null, a wrong
 *     ANSWER — and `-1` reads element 0xFFFFFFFF, which is not a wrong answer but a wild read. That is a
 *     reading of two functions and not a run; it is recorded here because it is the one argument for `0` that
 *     survives the measurements above, and because the sweep it argues for has to move the latches and the
 *     hand-resets in the same diff or it breaks the 41.
 * RETIREMENT: this paragraph goes when a realm slot's C type is JSClassID, because `-1` is then unspellable
 * and the question cannot be re-opened.
 *
 * WHAT THE COMPILER CANNOT CHECK HERE, SAID PLAINLY BECAUSE THE TOP OF THIS FILE PROMISES IT CAN. Every other
 * kind is separated by its slot's TYPE, so a miswired declaration does not compile. A realm slot is `int` and
 * so is an id, so this entry and agent_state_id have the same signature and a call routed to the wrong one is
 * silent — which is the type erasure at the root: realm_value_declare returns `int` for a JSClassID, and that
 * is what let a class id through the id door in the first place. Closing it is a signature change at 327 call
 * sites across 56 files (`realm_value_declare` 98, `realm_value_set` 118, `realm_value_get` 111, at
 * 8de85780) plus 75 statics, which is why it is not this diff. What stands in for the compiler meanwhile is
 * the assert in agent_state.c's own entry: a realm slot is a MINTED class id at the moment it is declared,
 * so it is `> 0` there, and a row declared above the line that assigns it fires.
 * RETIREMENT: this paragraph goes with the one above it, for the same reason.
 *
 * NAMED RESIDUAL — THE IDENTITY IS NOT ASSERTED, ONLY MADE DERIVABLE.
 *   NOT COVERED: a class id minted and never declared is COUNTED and not IMPOSSIBLE. This kind gives the
 *     registry the left-hand side of the conservation identity — the class ids it was told about, being its
 *     SLOT_CLASS rows plus its SLOT_REALM ones — and nothing here compares that against the allocator that
 *     handed them out, so nothing refuses a mint that skipped this registry.
 *   THE NEXT DIFF BUILDS: three things must exist afterward. (1) A way for the host to read how many class
 *     ids a runtime has handed out. `rt->js_class_id_alloc` is private to engine/qjs/quickjs.c and there is
 *     no public entry for it — `git grep -nE "JS_(GetClass(Count|IdAlloc)|ClassCount)" -- engine/qjs`
 *     answers 0 at 8de85780, and `js_class_id_alloc` occurs at three lines of quickjs.c and none of
 *     quickjs.h; grep both again before building, because that is a claim about a tree that moves. (2) A
 *     count over this registry of the two class-id kinds. (3) An equality between them, asserted where the
 *     declare column ends. It will not hold on the day it is written and that is the forcing function rather
 *     than a reason to defer it: what it names is every mint that never came through here, and closing those
 *     is a mint that DECLARES — one door taking the slot's address and the component's row, which is the root
 *     the identity exists to force.
 *   HOW ITS ABSENCE WOULD SHOW: `node engine/agentstate.mjs --rev <rev>` reports a nonzero UNDECLARED band
 *     in either channel while every dev build of that revision is silent. A gap a text sweep can see and no
 *     run of the engine can is the whole of what is missing; when the identity stands, the run sees it first.
 */
void agent_state_realm_slot_at(const char *component, const int *slot, const char *what,
                               const char *file, int line);                           /* pre-init: -1 */
#define agent_state_id(component, slot, what)    agent_state_id_at((component), (slot), (what), __FILE__, __LINE__)
#define agent_state_flag(component, slot, what)  agent_state_flag_at((component), (slot), (what), __FILE__, __LINE__)
#define agent_state_class(component, slot, what) agent_state_class_at((component), (slot), (what), __FILE__, __LINE__)
#define agent_state_atom(component, slot, what)  agent_state_atom_at((component), (slot), (what), __FILE__, __LINE__)
#define agent_state_value(component, slot, what) agent_state_value_at((component), (slot), (what), __FILE__, __LINE__)
#define agent_state_ptr(component, slot, what)   agent_state_ptr_at((component), (slot), (what), __FILE__, __LINE__)
#define agent_state_realm_slot(component, slot, what) \
    agent_state_realm_slot_at((component), (slot), (what), __FILE__, __LINE__)

/* How many slots this component declared — core/platform.c's row check, and nothing else. IT CANNOT ANSWER
   THE THIRD DIRECTION: a caller can only ask it about a name the caller already has, so 0 is returned both
   for a component that declared nothing and for a component whose slots were declared under a name that
   caller's list does not carry. Those are two different repairs, so they are two different questions. */
int  agent_state_count(const char *component);

/* THE REGISTRY, READ THE OTHER WAY: the `i`th declaration in declaration order, false past the end. This is
   the ONLY way to ask "is every declaration's component a real one?", because that question is asked of the
   REGISTRY and not of any list a caller holds. The three strings and the line are the ones the declaration was
   made with.
   THIS USED TO HAND BACK TWO STRINGS AND SAID THE ASSERT COULD THEREBY "name the exact line by its `what`
   rather than by an index into a table nobody can see". The second half of that is still right and the first
   was never true: `what` names the STATE, so it identifies the declaration only to a reader who already knows
   which file to open, and the walk this feeds is the one whose whole job is to say that the file is wrong
   about its row. The site is carried now, so the sentence is kept as the reason an index would have been
   worse rather than as a claim that a description is an address. */
bool agent_state_slot(int i, const char **component, const char **what,
                      const char **file, int *line);

/* EVERY DECLARED SLOT IS BACK AT ITS PRE-INIT VALUE. Run once, at the end of the release column. */
void agent_state_check_released(void);

/* THE INVERSE OF THIS COMPONENT'S DECLARATIONS, DERIVED FROM THEM — the LAST line of a component's release.
 *
 * WHY IT EXISTS. The release used to be the inverse of the declaration by being WRITTEN as one: a component
 * declared N slots in its `_init` and reset N slots in its `_free`, and the two lists were kept in step by
 * whoever remembered. That is a list maintained twice, in two functions a thousand lines apart, and the
 * failure mode is not a mistake anybody makes reading the code — it is a diff that ADDS a declaration to a
 * component that already has a release, touching only the `_init`. Measured, exactly once each way in one
 * session: a step id added to an existing component's `_init` with no matching line in its `_free` aborted TWO
 * stages of a build, and a new component landing the same week reset six of its seven slots and left the
 * seventh. Both were caught by agent_state_check_released — the mechanism works — and both were the same
 * clerical error, which is the kind a registry should not be asking a person to get right.
 *
 * SO THE UNDO IS COMPUTED FROM THE DECLARATIONS THEMSELVES. The registry already holds every slot's ADDRESS
 * and its KIND, and a kind IS a pre-init value — slot_is_pre_init reads exactly that pairing to decide the
 * check. This writes the same pairing. A component that declares a tenth slot therefore owes its release
 * NOTHING NEW, and the state where a declaration has no inverse cannot be reached by adding one.
 *
 * WHAT IT DOES NOT DO, AND WHY THE FORCING FUNCTION SURVIVES. It resets HANDLES, never references: freeing
 * what a slot names is the component's own work and stays in its `_free`, because only the component knows
 * whether a slot holds a JSValue, an atom, a malloc'd buffer or nothing at all. And this is called BY the
 * component rather than by core/platform.c's release column, which is not an oversight — a column that undid
 * every component automatically would make agent_state_check_released unable to fire at all, so a release
 * that never ran, or returned early, would stop being caught. One line per component is what leaves that
 * check something to say.
 *
 * IT IS THE LAST LINE, AND THE ORDER IS PART OF THE CONTRACT. A release that hands another component's claim
 * back, or asserts a claimant has already handed it back (§2.9's tree walk, §9.4.2's handler-set hook), is
 * asking about a slot this would null; called first, it would answer those DCHECKs itself. Free, assert, then
 * undo. */
/* THE NAMED RESIDUAL THAT STOOD HERE IS RETIRED — ITS GAP IS BUILT, AS agent_state_reached's PRECONDITION ON
 * THIS ENTRY. Its NOT-COVERED clause was exact and names what this entry now refuses: `an owner whose cascade
 * drops one member` — the member's values never freed and its handles put back anyway, so the release check
 * that exists to find exactly that was answered by THIS line instead of by the release.
 * ITS OTHER TWO CLAUSES WERE WRONG AND ARE RECORDED RATHER THAN DELETED, because a next-diff clause is read
 * ONCE, by somebody who has already decided to build it, so a wrong one is not caught — it is executed.
 *   - WHAT THE NEXT DIFF BUILDS said `a per-DECLARING-FILE undo, so the unit that resets is the unit that
 *     frees`. The unit that resets MAY NOT be the unit that frees, and this tree has already paid to learn
 *     that: a sub-component's release runs in the MIDDLE of its owner's cascade, so resetting there moves
 *     every one of its handles EARLIER. The whole bounds-class walk of DOM §5.3 "Interface AbstractRange" is
 *     declared as agent state in core/dom/abstract_range.c, and abstract_range_of hands each of its ids to
 *     JS_GetOpaque on every getter, so zeroing them from abstract_range_free — reached from range_free, with
 *     a dozen more releases and node_free's wrapper walk still to run — answers NULL for every live range,
 *     which range_pre_remove of DOM §5.5 "Interface Range" DCHECKs against. That file and core/dom/range.c
 *     each record that abort in their own words as the reason the reset MOVED here.
 *     Building the clause would have re-introduced it. The tell it carried is the one to copy: it named a
 *     MECHANISM, and a mechanism is the half of a residual that is a guess about this tree.
 *   - HOW ITS ABSENCE WOULD SHOW said an atom is `silent everywhere` because neither of JS_FreeRuntime's
 *     censuses sees one. An atom is the SUBJECT of the second: engine/qjs/quickjs.c's `[atomleak]` walk is
 *     UNCONDITIONAL, feeds the same leak flag as the object walk, and carries a DCHECK naming the survivor —
 *     which the first paragraph of THIS file already says, so the residual disagreed with its own header.
 *     What is genuinely silent is the POINTER: a malloc'd block reaches no census with an assert over it, and
 *     a ptr slot holding a HOOK this component installed into another leaves a DANGLING CALLBACK rather than
 *     memory, which nothing anywhere reports. SLOT_ID, SLOT_FLAG and SLOT_CLASS hold no reference at all, so
 *     what a dropped release loses there is whatever ELSE it did, and that is silent too.
 * RETIREMENT: this record goes when a residual in this tree cannot state a mechanism without the command that
 * greps its callers beside it, because the clause above is wrong in exactly the way that check would show.
 *
 * A COMPONENT NAME THAT DECLARED NOTHING IS AN ABORT and not a no-op, because the name is written twice — once
 * here and once at each declaration — and a silent no-op is the shape where the two spellings differ and the
 * release stops being the inverse of anything. THE CALLING SITE IS CARRIED for the reason the declaring
 * entries carry theirs: this abort's remedy is `one of these two spellings is wrong`, which names no file
 * unless the release's own address is in the message. THE BACKTICKS ARE NOT STYLE: that run is a SPELLING
 * BEING SHOWN and not a quotation of anything, so in double quotes it enters the citation auditor's quotation
 * channel and is judged against whatever standard is named NEAREST ABOVE it — which is a property of the
 * prose above, not of this line. It was reported as diverging from DOM the first time a citation was written
 * into the paragraph overhead. */
void agent_state_undo_at(const char *component, const char *file, int line);
#define agent_state_undo(component) agent_state_undo_at((component), __FILE__, __LINE__)

/* THE DECLARING FILE'S RELEASE RAN — THE ONE FACT THE UNDO ABOVE CANNOT KNOW AND MAY NOT PUT A SLOT BACK
 * WITHOUT. A row is declared from SEVERAL FILES, because a sub-component names the row whose release reaches
 * it rather than its own file (the top of this header says why), and the cascade that reaches those files is
 * a TREE and not a list: `element`'s reaches range_free, which reaches abstract_range_free. The undo runs
 * ONCE, at the row's last line, and resets every slot carrying the row's name — so an owner that drops one
 * member has that member's handles put back by a release that never reached it, and the release check finds
 * them already reset. It is the one direction that walk is structurally blind to, and only for a row that
 * ends in the undo.
 *
 * SO EACH DECLARING FILE SAYS SO ITSELF, AS THE LAST LINE OF ITS OWN `_free`, and the undo REFUSES a slot
 * declared in a file that has not. This is a CLAIM and not a reset — it writes no slot, which is the whole
 * reason it can be made in the middle of a cascade. Nothing moves: the reset stays at the row's last line,
 * where agent_state_undo's own ordering contract requires it and where this tree has already established
 * that it must be.
 *
 * THE UNDO'S OWN FILE IS EXEMPT, AND THAT IS NOT A SPECIAL CASE BUT THE SAME RULE: the call to the undo IS
 * that file's claim, made in the same breath as the reset it is a claim about. A row whose undo lives in a
 * file that declares nothing under it has an EMPTY exemption, so every declaring file must have spoken —
 * which is the stricter reading and needs no second check to produce.
 *
 * IT IS OWED ONLY WHERE THE UNDO IS, AND THAT IS WHY IT IS NOT AN OBLIGATION ON EVERY DECLARATION. A row that
 * hand-resets already leaves a dropped member's slots SET for agent_state_check_released to find — the claim
 * at the top of this file, and true for those rows — so there is no wrong reset there to refuse. The
 * population is one line in each file that declares under a row whose release ends in the undo and is not
 * that file; `agent_state_slot` hands back the declaring file of every slot, so it is derivable and is not a
 * list anybody keeps.
 *
 * THE COMPONENT IS NAMED HERE FOR THE REASON THE UNDO NAMES IT, AND NOT TO SELECT ANYTHING: the row is
 * spelled once at each declaration and once at each release that claims it, and a sub-component claiming the
 * WRONG row is the state those two spellings exist to separate. The pair selects, so a file that comes to
 * declare under two rows is handled without anybody noticing; what the name buys is that a release naming a
 * row it declares nothing under cannot pass silently. */
void agent_state_reached_at(const char *component, const char *file, int line);
#define agent_state_reached(component) agent_state_reached_at((component), __FILE__, __LINE__)

/* The registry is the AGENT's, like everything on it. */
void agent_state_reset(void);

#endif
