/* HTML §8.9.1 Simple dialogs — `alert()`, `alert(message)`, `confirm(message)`, `prompt(message, default)`.
 * See simple_dialogs.c for the steps; this file states the two decisions the steps leave to the user agent,
 * because both of them are the ones a headless engine gets wrong.
 *
 * THE FIRST IS "WE CANNOT SHOW SIMPLE DIALOGS", AND DECLINING ITS OPTIONAL ARMS IS THE WHOLE COMPONENT.
 * §8.9.1's algorithm has four steps and only the first is forced: "If the active sandboxing flag set of
 * window's associated Document has the sandboxed modals flag set, then return true." The second is optional
 * (the event loop's termination nesting level) and the third is optional outright — "Optionally, return true.
 * (For example, the user agent might give the user the option to ignore all modal dialogs, and would thus abort
 * at this step whenever the method was invoked.)" — and it is the arm a headless browser takes: no display, no
 * user, so `confirm()` answers false and `prompt()` answers null for every page forever.
 *   THIS ENGINE DECLINES IT, and the reason is CLAUDE.md §Headless in its own words: "the only missing piece is
 * a physical IO device; the spec still defines behavior without one", and "never collapse a modelable value to
 * bare-concrete either — that deletes the fork and its coverage". Taking the optional arm is exactly that
 * collapse: `if (confirm("Delete this account?"))` would have ONE feasible arm in a bundle where both are real
 * programs, and the arm it deletes is the one behind the gate — the delete endpoint, the destructive request,
 * the code a logged-out crawl never reaches. So the only arm this component takes is the SANDBOXED one, which
 * is a fact the model actually holds (core/frame/sandboxing.h's SANDBOX_MODALS, cleared by `allow-modals`).
 *
 * THE SECOND IS WHAT THE USER ANSWERS, AND IT IS UNKNOWN EXTERNAL STATE RATHER THAN AN ABSENT DEVICE. Having
 * declined "cannot show", the dialog IS shown and the user DOES respond, and what they respond is not
 * something this engine has observed — it is the most general concolic value, which is what core/html/
 * input_picker.c already mints for the file dialog's `dismissed` and what core/html/user_activation.h mints
 * for "has this user ever interacted". `confirm` answers a concolic boolean and `prompt` a concolic string, so
 * the page's own branch on the answer forks and BOTH worlds run — and neither member needs a fork of its own,
 * because neither algorithm has a C `if` over the answer: they return it.
 *   THE EXAMPLE EACH CARRIES IS THE ANSWER OF A USER WHO ACCEPTS THE DIALOG AS OFFERED — `true` for a confirm,
 * and for a prompt the `default` argument, because §8.9.1 says the response "must be defaulted to the value
 * given by default" in those words, which makes it a value the PAGE determined and not one this engine
 * invented (CLAUDE.md §@H-must-never-invent draws the line exactly there). A `prompt` whose default is itself
 * unknown external input carries NO example, which is the honest state rather than a hole: there is no
 * concrete string the page computed for the response to be defaulted to.
 *   WHERE NO SOURCE OVERLAY IS INSTALLED — a conformance host — concolic_source_wrap hands the example back
 * unchanged, so `confirm()` is `true` and `prompt(m, d)` is `d` there. Both are conformant answers for a user
 * agent whose user accepts, and they are DIFFERENT from what a headless browser reports, because a headless
 * browser is taking the optional arm this component declined. That divergence is the point of the component,
 * so it is stated here rather than discovered.
 *
 * WHAT "SHOW MESSAGE TO THE USER" IS, WITH NO DISPLAY. The same thing Console Standard §2.3 Printer is: one
 * line on the engine's diagnostic stream, written through core/json_buf.h so the page's own bytes cannot end
 * the line early and forge a record. console.c states the argument and it holds here unchanged — "a host with
 * nowhere to show this still runs every step above it", and the steps above the show (the sandboxing guard,
 * Infra §4.7 Strings' normalize newlines, §8.9.1's optionally truncate) are real regardless. What is NOT
 * performed is the step after it, "Optionally, pause while waiting for the user to acknowledge the message":
 * it is optional, and a pause awaiting a user who cannot answer never ends, which would be a flow driven to
 * completion by a wait no scheduler could rank.
 *
 * THERE IS NO WEBDRIVER BiDi SESSION HERE, so §8.9.1's `userPromptHandler` is "none" at all three members and
 * the "none" arm is the one that runs. The two invocations that bracket it (user prompt opened / user prompt
 * closed) reach no session and are not written as C that pretends otherwise. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_SIMPLE_DIALOGS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_SIMPLE_DIALOGS_H

#include "quickjs.h"

/* Declared once per AGENT: §8.9.1's three member declarations. */
void simple_dialogs_init(JSContext *ctx);

/* §8.9.1's three Window members, on THIS realm's global. They are not on HTML §7.2.1.3.1 CrossOriginProperties'
   list of cross-origin accessible window property names, so unlike §6.6.6's `focus`/`blur` there is no second
   surface to install them on — a cross-origin `otherW.alert()` is a SecurityError and must stay one. */
void simple_dialogs_install(JSContext *ctx, JSValueConst global);

void simple_dialogs_free(void);

#endif
