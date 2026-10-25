/* solver/solve_joint.c — the contexts one delivered value reaches (see solve_joint.h). */
#include "solver/solve_joint.h"
#include "solver/solve_js.h"
#include "check.h"
#include <stdio.h>
#include <string.h>

/* Does every sibling context admit `payload` (solve_js_admits)? A NULL joint admits everything. */
static int joint_admits(const SolveJoint *j, const char *payload)
{
    DCHECK(payload != NULL, "a joint admissibility question was asked of no payload");
    if (!j) return 1;
    for (int i = 0; i < j->njs; i++)
        if (!solve_js_admits(j->js[i], payload)) return 0;
    return 1;
}

const char *solve_joint_after_exit(const SolveJoint *j, const char *exit)
{
    char resumed[64];
    int k;

    DCHECK(exit != NULL && *exit, "a joint continuation was asked after no exit");
    if (joint_admits(j, exit)) return "";
    k = snprintf(resumed, sizeof resumed, "%s%s", exit, SOLVE_JS_INERT_RESUME);
    CHECK(k > 0 && (size_t)k < sizeof resumed, "solve_joint: an exit and its resume did not fit");
    return joint_admits(j, resumed) ? SOLVE_JS_INERT_RESUME : NULL;
}
