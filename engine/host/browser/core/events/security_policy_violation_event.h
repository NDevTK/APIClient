/* SecurityPolicyViolationEvent — Content Security Policy Level 3 §5.1 "Violation DOM Events".
   See security_policy_violation_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_SECURITY_POLICY_VIOLATION_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_SECURITY_POLICY_VIOLATION_EVENT_H

#include <stdint.h>

#include "quickjs.h"

void security_policy_violation_event_init(JSContext *ctx);       /* the slot key + the IDL declarations */
/* §3.7: THIS REALM's prototype AND interface object. Declared into realm.h's one list by the init above. */
void security_policy_violation_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT, through core/events/event.c's event_free_subclasses — see hash_change_event.h for
   why every Event subclass gives its slots back through core/platform.c's `event` row rather than its own. */
void security_policy_violation_event_free(JSRuntime *rt);

/* THERE IS NO `..._new`, AND THAT IS DOM §4.5's TABLE RATHER THAN A GAP HERE. §4.5 "createEvent" names
   BeforeUnloadEvent, CompositionEvent, CustomEvent, DeviceMotionEvent, DeviceOrientationEvent, DragEvent,
   Event, FocusEvent, HashChangeEvent, KeyboardEvent, MessageEvent, MouseEvent, StorageEvent, TextEvent,
   TouchEvent and UIEvent — and not this one, so `document.createEvent('SecurityPolicyViolationEvent')` is
   step 3's NotSupportedError in every browser and a row here would make this engine answer where the spec
   throws. It is exactly the argument core/events/create_event.c already records for PageTransitionEvent. */

/* CSP §5.5's ATTRIBUTE LIST, AS THE ONE STRUCT ITS FIRE FILLS — every `with its attributes initialized as
 * follows` row of that algorithm, in the order §5.1 declares them, and nothing else.
 *
 * IT IS A STRUCT AND NOT ELEVEN ARGUMENTS because the list is twelve attributes long and eleven positional
 * `const char *`s in a row is a call nobody can read and every caller is free to mis-order — the defect
 * core/events/event_target.h records for its own fire buffer, arriving in an argument list instead of in an
 * array width. A named field cannot be passed in the wrong position.
 *
 * IT CARRIES NO `violatedDirective`, AND THAT IS §5.5's OWN NOTE RATHER THAN A FIELD DROPPED: "Both
 * effectiveDirective and violatedDirective are the same value. This is intentional to maintain backwards
 * compatibility." Two fields could disagree; one cannot, and the interface still declares both members
 * because the IDL does.
 *
 * EVERY STRING IS BORROWED AND NUL-TERMINATED, and lives only across the call — the mint copies into the
 * event's own slots, because an event outlives the algorithm that composed it and a slot holding a caller's
 * stack pointer is a use-after-free one fire later.
 *
 * `source_file` MAY BE NULL, which is §5.5's "if violation's source file is not null, or null otherwise" —
 * and §5.1 types the member `USVString` rather than `USVString?`, so a null has no representation on the
 * interface and the attribute answers the EMPTY STRING. That is what the one non-nullable row of a nullable
 * spec value means, and it is stated here rather than left to each caller to decide. No other field may be
 * NULL: §2.4 gives `sample` the empty string as its initial value and every other row a real string. */
typedef struct {
    const char *document_uri;          /* §5.4 over violation's url */
    const char *referrer;              /* §5.4 over violation's referrer */
    const char *blocked_uri;           /* §5.2 over violation's resource */
    const char *effective_directive;   /* violation's effective directive — AND violatedDirective */
    const char *original_policy;       /* the serialization of violation's policy */
    const char *source_file;           /* §5.4 over violation's source file, or NULL */
    const char *sample;                /* violation's sample */
    const char *disposition;           /* §5.1's SecurityPolicyViolationEventDisposition — "enforce"/"report" */
    uint16_t    status_code;           /* violation's status */
    uint32_t    line_number;           /* violation's line number */
    uint32_t    column_number;         /* violation's column number */
} SecurityPolicyViolationEventFields;

/* CSP §5.5's "fire an event named securitypolicyviolation that uses the SecurityPolicyViolationEvent
   interface at target with its attributes initialized as follows" — the EVENT, minted rather than fired,
   because §5.5 wraps the whole dispatch in "Queue a task to run the following steps" and
   core/events/event_target.h's `event_target_fire` is that queued reach. isTrusted is TRUE.
   `bubbles` and `composed` are BOTH TRUE and are this algorithm's rather than a caller's — §5.5 writes them
   into the same initializer list as the twelve attributes, and its note says why composed is set: "this event
   can be captured on its way into, and will bubble its way out of, a shadow tree". `cancelable` is not in
   that list, so it is false. Returns a new owned SecurityPolicyViolationEvent. */
JSValue security_policy_violation_event_new_to_fire(JSContext *ctx,
                                                    const SecurityPolicyViolationEventFields *f);

#endif
