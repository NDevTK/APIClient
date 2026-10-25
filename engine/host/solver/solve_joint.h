/* solver/solve_joint.h — the contexts one delivered value reaches beside the sink a breakout is derived for.
 *
 * A candidate is delivered once at its source, and every read of that source answers it (solver/concolic.h),
 * so a markup breakout delivered through `location.hash` is also what an earlier `eval("'" + location.hash +
 * "'")` reads. A breakout that makes that eval throw ends the script before the markup write runs, in the
 * engine and in a real browser alike. So a breakout is derived jointly: each other context of the same root
 * states what it admits, and the derivation picks bytes every context accepts.
 *
 * The other contexts are the witnesses of the sibling searches over the same delivery root: the strings their
 * own probes' runs handed their sinks, each holding its class's locator where the value landed. Only script
 * contexts constrain today; a markup context parses every byte sequence (HTML §13.2 rejects no input).
 * Whether another context runs before the sink is the page's order, so a joint spelling is derived beside the
 * sink's own and the fire decides which one the page lets through. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_JOINT_H
#define ENGINE_HOST_SOLVER_SOLVE_JOINT_H

/* The eval witnesses of a search's siblings, borrowed for one derivation. */
typedef struct { const char *const *js; int njs; } SolveJoint;

/* What must follow `exit` (a breakout's first bytes, the exit of its own sink's state) for every sibling to
   admit it: "" when no sibling's state is left by `exit`, SOLVE_JS_INERT_RESUME when some is and every
   sibling admits it followed by that resume, and NULL when no continuation keeps them all parsing. */
const char *solve_joint_after_exit(const SolveJoint *j, const char *exit);

#endif
