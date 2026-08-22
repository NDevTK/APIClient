/* CONSTRAINT VALIDATION — HTML §4.10.21. See constraint_validation.c.
 *
 * WHY IT IS ITS OWN COMPONENT. §4.10.21.1's ten VALIDITY STATES are a question asked of an ELEMENT — the
 * `validity` and `willValidate` members ask it, `checkValidity()` and `reportValidity()` ask it, and
 * §4.10.22.3 step 5.4 asks it of every control of a form on its way to a request. Written inside the
 * submission it would be the submission's private opinion of what a valid control is, which is exactly how the
 * same rule ends up spelled twice and answered two ways. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CONSTRAINT_VALIDATION_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CONSTRAINT_VALIDATION_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* §4.10.21.1's TEN VALIDITY STATES, as ONE list expanded twice — the bit an element's validity is reported in,
   and the `ValidityState` attribute that reports it. The list is here rather than beside one of its readers
   because it has two: this component computes them for a BUILT-IN control from its value and its constraint
   attributes, and §4.13.7.3's `setValidity` WRITES them for a form-associated custom element. Those two are one
   set of flags and element_internals.c holds its own copy of the list today; the day it exports a reader for
   the flags it wrote (see constraint_validation.c's DFAIL, which names it) that copy deletes and this is the
   one declaration both halves answer from. */
#define CONSTRAINT_VALIDATION_STATES(X) \
    X(CV_VALUE_MISSING,    "valueMissing") \
    X(CV_TYPE_MISMATCH,    "typeMismatch") \
    X(CV_PATTERN_MISMATCH, "patternMismatch") \
    X(CV_TOO_LONG,         "tooLong") \
    X(CV_TOO_SHORT,        "tooShort") \
    X(CV_RANGE_UNDERFLOW,  "rangeUnderflow") \
    X(CV_RANGE_OVERFLOW,   "rangeOverflow") \
    X(CV_STEP_MISMATCH,    "stepMismatch") \
    X(CV_BAD_INPUT,        "badInput") \
    X(CV_CUSTOM_ERROR,     "customError")
#define CV_STATE_ENUM(id, name) id,
#define CV_STATE_NAME(id, name) name,
enum { CONSTRAINT_VALIDATION_STATES(CV_STATE_ENUM) CV_STATE_COUNT };

/* Declared once per AGENT (the custom-validity slot key and the member ids), installed per realm on the
   interfaces that DECLARE §4.10.21.3's members. Both are reached from §4.10's declaration point, because that
   is where the prototypes these go on arrive. */
void constraint_validation_declare(JSContext *ctx);
void constraint_validation_install(JSContext *ctx, JSValueConst input_proto, JSValueConst textarea_proto);
void constraint_validation_free(JSRuntime *rt);

/* §4.10.21.1's "a submittable element is a CANDIDATE FOR CONSTRAINT VALIDATION except when a condition has
   barred the element from constraint validation", and `willValidate`'s whole answer. */
bool constraint_validation_is_candidate(JSContext *ctx, JSValueConst wrap);

/* §4.10.21.2's "INTERACTIVELY VALIDATE THE CONSTRAINTS of form", as the sub-sequence a caller's machine
   performs — the shape §4.10.22.4's entry-list construction already has, and for the same two reasons. It
   SUSPENDS because step 5.1 fires an `invalid` event at every unsatisfied control, which is the page's code;
   and it YIELDS per control, because both of its walks are of the PAGE's size.
   The state is OPAQUE and heap-allocated: it holds a live regexp match (§4.10.5.3.6's compiled pattern is a
   real RegExp and a match parks at its back-edges like any other loop), and a caller that had to see one would
   need libregexp's context in its own frame. `*slot` starts NULL — a js_mallocz'd caller state already is. */
typedef struct ConstraintValidationRun ConstraintValidationRun;

/* WHAT THIS RUN OWNS, forwarded from the calling machine's own `visit` and its own teardown. A fork mid-walk
   gives each arm its own invalid-controls list and its own half-finished match. */
void constraint_validation_visit(JSContext *ctx, ConstraintValidationRun **slot, JSStepVisit *v);
void constraint_validation_release(JSContext *ctx, ConstraintValidationRun **slot);

/* §4.10.21.2 itself. `h` is the CALLING machine's header, because a control whose value is unknown external
   input makes "does this control satisfy its constraints" a genuine two-outcome question and the answer is an
   outcome FORK rather than a guess. Returns JS_STEP_CALL / JS_STEP_YIELD / JS_STEP_FORK (the caller returns
   it), -1 with the throw live, or 0 once `*ppositive` holds the result: true for §4.10.21.2's POSITIVE result
   (every control in the form is valid), false for its negative one. */
int constraint_validation_interactively_run(JSContext *ctx, JSStepHdr *h, ConstraintValidationRun **slot,
                                            JSValueConst form, JSValue in, bool *ppositive,
                                            JSValue **out_cb, int *out_argc);

#endif
