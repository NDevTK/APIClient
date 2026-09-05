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
 * THE KIND IS THE PRE-INIT VALUE. `-1` for an id (a pool entry, a step definition, a realm slot), `0` for a
 * class id or a flag, JS_ATOM_NULL for an interned name, JS_UNDEFINED for a JSValue, NULL for a pointer — the
 * same values C gives a static before anything runs, except where the component's own initialiser states
 * otherwise. There is one function per kind rather than one function and an enum, so the COMPILER checks that
 * the slot really is what its declaration says it is.
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
void agent_state_id(const char *component, const int *slot, const char *what);        /* pre-init: -1 */
void agent_state_flag(const char *component, const int *slot, const char *what);      /* pre-init: 0 */
void agent_state_class(const char *component, const JSClassID *slot, const char *what);/* pre-init: 0 */
void agent_state_atom(const char *component, const JSAtom *slot, const char *what);   /* pre-init: JS_ATOM_NULL */
void agent_state_value(const char *component, const JSValue *slot, const char *what); /* pre-init: JS_UNDEFINED */
/* A POINTER SLOT — a recorded JSRuntime, a malloc'd buffer, a hook this component installed into another. The
   address crosses as `const void *` and the check compares the BYTES of a null pointer, because reading a
   `JSRuntime *` object through a `void *` lvalue is the strict-aliasing violation CLAUDE.md's §C-stack rule
   was written about; memcmp reads unsigned chars and is legal for every object there is. */
void agent_state_ptr(const char *component, const void *slot, const char *what);

/* How many slots this component declared — core/platform.c's row check, and nothing else. IT CANNOT ANSWER
   THE THIRD DIRECTION: a caller can only ask it about a name the caller already has, so 0 is returned both
   for a component that declared nothing and for a component whose slots were declared under a name that
   caller's list does not carry. Those are two different repairs, so they are two different questions. */
int  agent_state_count(const char *component);

/* THE REGISTRY, READ THE OTHER WAY: the `i`th declaration in declaration order, false past the end. This is
   the ONLY way to ask "is every declaration's component a real one?", because that question is asked of the
   REGISTRY and not of any list a caller holds. The two strings are the ones the declaration was made with, so
   the assert that fires can name the exact line by its `what` rather than by an index into a table nobody
   can see. */
bool agent_state_slot(int i, const char **component, const char **what);

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
 * undo.
 *
 * A COMPONENT NAME THAT DECLARED NOTHING IS AN ABORT and not a no-op, because the name is written twice — once
 * here and once at each declaration — and a silent no-op is the shape where the two spellings differ and the
 * release stops being the inverse of anything. */
void agent_state_undo(const char *component);

/* The registry is the AGENT's, like everything on it. */
void agent_state_reset(void);

#endif
