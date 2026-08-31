/* HTML §8.9.1 Simple dialogs. See simple_dialogs.h for the two user-agent decisions this component makes and
   for why declining §8.9.1's optional "we cannot show simple dialogs" arms is the whole of it. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/dom/document.h"
#include "core/frame/sandboxing.h"
#include "core/html/simple_dialogs.h"
#include "core/idl_args.h"
#include "core/json_buf.h"
#include "solver/concolic.h"

static JSRuntime *g_rt;
static int g_id_alert = -1, g_id_confirm = -1, g_id_prompt = -1;

/* WHAT THE USER ANSWERED, AS A SOURCE — the shape a finding renders and the identity a flow's constraint is
   keyed by. Two members, two questions, two identities: a page that has narrowed one has said nothing about
   the other, and one key shared between them would let a `confirm` the flow answered decide a `prompt` it has
   not reached. The spelling follows core/html/input_picker.c's file-dialog pair, which is the same kind of
   fact about the same kind of dialog. */
#define SD_CONFIRM_SHAPE "{confirm() accepted}"
#define SD_CONFIRM_SRC   "window.confirm().accepted"
#define SD_PROMPT_SHAPE  "{prompt() response}"
#define SD_PROMPT_SRC    "window.prompt().response"

/* §8.9.1's "we cannot show simple dialogs for a Window window", which is an algorithm of four steps and not a
 * property of the display. Step 1 is the only forced one — "If the active sandboxing flag set of window's
 * associated Document has the sandboxed modals flag set, then return true" — and it is a fact this model holds:
 * core/frame/sandboxing.h parses `allow-modals` out of the `sandbox` content attribute and §7.1.5 lists
 * `alert`, `confirm`, `print` and `prompt` among the modal dialogs the flag suppresses.
 *
 * STEPS 2 AND 3 ARE DECLINED AND THE DECLINING IS THE DESIGN. Step 2 reads the event loop's termination nesting
 * level and says "then OPTIONALLY return true"; step 3 is "Optionally, return true" with no condition at all.
 * Taking either is what makes a headless browser answer `false`/`null` to every dialog a page ever opens, and
 * CLAUDE.md §Headless forbids exactly that collapse — a bare-concrete answer here deletes the arm behind the
 * gate, which in a real bundle is the destructive request or the admin route. So this returns false wherever
 * the sandbox has not spoken, and the answer the user gives is minted as unknown state instead.
 *
 * IT IS THE REALM'S DOCUMENT AND NOT A REMEMBERED ONE. Web IDL §3.7.3 Interface prototype object makes every
 * member of a [Global] interface an OWN property of the global, so each realm installs its own function object
 * and js_call_c_function takes `ctx` off it — an `<iframe sandbox>`'s `alert` is a different object from its
 * parent's and answers out of its own Document's flag set, which is what §8.9.1's "for this" means for every
 * call a page makes by naming the member on the window it belongs to. */
static bool sd_cannot_show(JSContext *ctx)
{
    return (document_active_sandbox_flags(ctx) & SANDBOX_MODALS) != 0;
}

/* INFRA §4.7 Strings' NORMALIZE NEWLINES: "replace every U+000D CR U+000A LF code point pair with a single
   U+000A LF code point, and then replace every remaining U+000D CR code point with a U+000A LF code point."
   The two replacements are written as one pass rather than two because the second is defined over what the
   first left — a lone CR — and a pass that ran them in the order the sentence lists them would turn a CRLF
   into two LFs on the second sweep. It is NOT core/html/form_data.c's fd_normalize_newlines: that one is HTML
   §4.10.22.8's multipart/form-data encoding algorithm step 1 and normalizes toward CRLF, the opposite
   direction, which is why one shared helper would be two algorithms wearing one name.
   The result is at most as long as the input. Caller frees. */
static char *sd_normalize_newlines(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    size_t i, w = 0;

    CHECK(out != NULL, "simple dialogs: OOM normalizing a dialog message's newlines");
    for (i = 0; i < n; i++) {
        if (s[i] == '\r') {
            out[w++] = '\n';
            if (i + 1 < n && s[i + 1] == '\n') i++;   /* the CR LF PAIR is one code point after the first rule */
        } else {
            out[w++] = s[i];
        }
    }
    out[w] = 0;
    return out;
}

/* §8.9.1's "Show message to the user, treating U+000A LF as a line break", performed by a user agent with no
 * display: ONE line on the engine's diagnostic stream, which is what Console Standard §2.3 Printer's
 * implementation-defined printer already is in this engine and for the reason console.c states — "a host with
 * nowhere to show this still runs every step above it".
 *
 * THE PAGE'S BYTES NEVER LEAVE THE STRING. The diagnostic channel carries records a reader and
 * engine/build.mjs both parse, so a message pasted onto it raw would let a page FORGE one, newline and all;
 * core/json_buf.h escapes the quote, the backslash and every C0 byte, so one call is one line and the LF this
 * step is required to treat as a line break cannot end the record early. That is also why the treatment is
 * `\n` INSIDE a JSON string rather than a real newline in the output: the record is one line, and its reader
 * gets the line breaks the page wrote.
 *
 * `dialog` IS §8.9.1'S OWN WORD FOR WHICH DIALOG THIS IS — the same string the algorithm hands to WebDriver BiDi
 * user prompt opened ("alert", "confirm", "prompt"), so a reader counting dialogs is counting what the standard
 * names rather than a vocabulary invented here. */
static void sd_show(const char *dialog, const char *message)
{
    JsonBuf b = { 0 };
    char *line;

    DCHECK(dialog != NULL && message != NULL,
           "simple dialogs: a dialog was shown with no name or no message — §8.9.1 step 2 gives every member a "
           "message before the show, and the name is a literal at each of the three call sites");
    json_buf_raw(&b, "@DIALOG {");
    json_buf_key(&b, "dialog");
    json_buf_str(&b, dialog);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "message");
    json_buf_str(&b, message);
    json_buf_raw(&b, "}\n");
    line = json_buf_take(&b);
    CHECK(line != NULL, "simple dialogs: the dialog's line could not be allocated");
    fputs(line, stderr);
    free(line);
    json_buf_free(&b);
}

/* §8.9.1 STEPS 3 AND 4, over one argument, as the one function all three members reach them through.
 *
 * "Set message to the result of normalizing newlines given message. Set message to the result of optionally
 * truncating message." The second is PERFORMED and not skipped: "To optionally truncate a simple dialog string
 * s, return either s itself or some string derived from s that is shorter", and this user agent returns s
 * itself — which is the option §8.9.1 names first, and the only one that does not hide part of what the page
 * said from the record below. (Its note explains what the other option is for: limiting the abuse potential of
 * a dialog a person is looking at. There is no person looking at this one.)
 *
 * AN UNKNOWN MESSAGE IS STILL A MESSAGE. core/idl_args.h's `idl_concolic_rule` answers IDL_CONCOLIC_CROSSES
 * for a Web IDL §3.2.10 DOMString position, so unknown external input reaches this body AS ITSELF and
 * `alert(location.hash)` arrives carrying no string — and concolic_name_cstr answers the
 * value's SHAPE, a real stable string, which is what every other DOM member that takes a name does with one
 * (core/css/media_query_list.c's `matchMedia` is the pattern). The record then says `{location.hash}` where a
 * ToString would have said something the page never computed.
 *
 * Returns an allocated string the caller frees, or NULL with a throw live. */
static char *sd_dialog_string(JSContext *ctx, JSValueConst v)
{
    const char *s = concolic_name_cstr(ctx, v);
    char *out;

    if (!s) return NULL;
    out = sd_normalize_newlines(s, strlen(s));   /* step 3 */
    JS_FreeCString(ctx, s);
    return out;                                  /* step 4, with `s itself` as the truncation */
}

/* §8.9.1's `alert()` and `alert(message)`.
 *
 * TWO OVERLOADS AND NOT AN OPTIONAL ARGUMENT, which is the declaration this file makes and which step 2 then
 * reads back: "If the method was invoked with no arguments, then let message be the empty string; otherwise,
 * let message be the method's first argument." §8.9.1's own note says why it is written that way and what the
 * difference is — "This method is defined using two overloads, instead of using an optional argument, for
 * historical reasons. The practical impact of this is that alert(undefined) is treated as alert("undefined"),
 * but alert() is treated as alert("")" — so the declaration makes position 0 optional with NO default value,
 * and an `undefined` the page actually passed is converted rather than being read as an absent argument. A
 * declared `= ""` would have made those two calls the same call, which is the one thing the note says they are
 * not. */
static JSValue js_sd_alert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    char *message;

    (void)this_val; (void)magic;
    if (sd_cannot_show(ctx)) return JS_UNDEFINED;                       /* step 1 */
    DCHECK(argc == 0 || JS_IsString(argv[0]) || concolic_is(argv[0]),
           "§8.9.1's `alert` reached its body with a first argument that is neither a String nor unknown "
           "external input — the position is declared DOMString and the argument machine runs Web IDL §3.2.10 "
           "DOMString's conversion before the body, so a third kind is a declaration that has come apart from "
           "this body");
    message = argc == 0 ? sd_normalize_newlines("", 0) : sd_dialog_string(ctx, argv[0]);   /* steps 2-4 */
    if (!message) return JS_EXCEPTION;
    sd_show("alert", message);   /* steps 5-7, with `userPromptHandler` "none" — see simple_dialogs.h */
    free(message);
    /* "Optionally, pause while waiting for the user to acknowledge the message" is DECLINED, so nothing here
       stalls the tasks and microtasks §8.9 says a dialog stalls. */
    return JS_UNDEFINED;
}

/* §8.9.1's `confirm(message)`. Steps 1-5 are the alert's, and then: "Let accepted be false. If
 * userPromptHandler is 'none': Pause until the user responds either positively or negatively. If the user
 * responded positively, then set accepted to true. … Return accepted."
 *
 * WHAT THE USER RESPONDED IS THE UNKNOWN, AND THE PAUSE IS WHERE A HEADLESS ENGINE LOSES IT. There is no user
 * to wait for, so the wait cannot happen; what the wait would have produced is a fact about the outside world
 * that this run has not observed, which is the most general concolic value. Returning it rather than a decided
 * boolean is what keeps `if (confirm(…))` a fork — and the fork is not asked HERE, because this algorithm has
 * no `if` over `accepted`: step 10 returns it, and the page's own branch is where the two worlds are born. A
 * member that forked its own return would be minting two flows to answer one question twice.
 *
 * THE EXAMPLE IS `true`, the answer of a user who accepts the dialog as offered, and it marks the arm that runs
 * first rather than the arm that runs alone. Both are explored; see simple_dialogs.h for why the accepting
 * answer is the example and for what a conformance host sees. */
static JSValue js_sd_confirm(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    char *message;
    JSValue accepted;

    (void)this_val; (void)magic;
    if (sd_cannot_show(ctx)) return JS_FALSE;                           /* step 1 */
    DCHECK(argc == 1, "§8.9.1's `confirm` reached its body at an arity its declaration does not produce — "
                      "`optional DOMString message = \"\"` is declared with §3.6 steps 15.4.1 and 16.1's "
                      "default, so the position is filled whether or not the page wrote it");
    message = sd_dialog_string(ctx, argv[0]);                           /* steps 2-3 */
    if (!message) return JS_EXCEPTION;
    sd_show("confirm", message);                                   /* step 4 */
    free(message);
    accepted = concolic_source_wrap(ctx, SD_CONFIRM_SHAPE, SD_CONFIRM_SRC, JS_TRUE);   /* steps 5-10 */
    CHECK(!JS_IsException(accepted), "simple dialogs: a confirm's response could not be allocated");
    return accepted;
}

/* §8.9.1's `prompt(message, default)`. Step 5 is the one that differs from the confirm's: "Show message to the
 * user, treating U+000A LF as a line break, and ask the user to either respond with a string value or abort.
 * The response must be defaulted to the value given by default."
 *
 * ONE UNKNOWN, NOT TWO. The steps read as two facts — whether the user aborted (step 7's `result` starts null)
 * and what they typed (step 8.2) — but a page cannot observe them separately: it observes ONE returned value
 * that is either null or a string. So this is one concolic with one identity, and `r === null` is the page's
 * own branch over it, forked by the ordinary branch machinery. Two sources here would fork twice for one
 * question and let a flow that decided "did not abort" say nothing about the value it then read.
 *
 * THE EXAMPLE IS THE `default` THE PAGE SUPPLIED, because step 5 says the response is defaulted to it — so a
 * user who accepts what the dialog offers returns exactly those bytes, and they are bytes the PAGE computed.
 * That is the line CLAUDE.md §@H draws between a value the code determined and one the solver invented, and it
 * is what lets `fetch("/u/" + prompt("name", "guest"))` carry `/u/guest` instead of a hole.
 *   A DEFAULT THAT IS ITSELF UNKNOWN CARRIES NO EXAMPLE, and that is a positive statement rather than a
 * fallback: there is no concrete string the page determined for the response to be defaulted to, so the
 * response is the unconstrained unknown, which is what core/html/media_element.c mints for a resource nobody
 * has read. Choosing the unknown's own shape text as the example would report a value no run ever computed. */
static JSValue js_sd_prompt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    char *message;
    JSValue result;

    (void)this_val; (void)magic;
    if (sd_cannot_show(ctx)) return JS_NULL;                            /* step 1 */
    DCHECK(argc == 2, "§8.9.1's `prompt` reached its body at an arity its declaration does not produce — both "
                      "positions are `optional DOMString … = \"\"` and §3.6 steps 15.4.1 and 16.1 place the "
                      "default at each, so both are filled whether or not the page wrote them");
    message = sd_dialog_string(ctx, argv[0]);                           /* steps 2-3 */
    if (!message) return JS_EXCEPTION;
    /* STEP 4, "Set default to the result of optionally truncating default", with `s itself` as the truncation
       exactly as at every other position — so the value handed to step 5 is the one the page wrote, and the
       example below is that value and not a derivation of it. */
    sd_show("prompt", message);                                    /* step 5 */
    free(message);
    result = concolic_source_wrap(ctx, SD_PROMPT_SHAPE, SD_PROMPT_SRC,
                                  JS_IsString(argv[1]) ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED);
    CHECK(!JS_IsException(result), "simple dialogs: a prompt's response could not be allocated");
    return result;                                                      /* steps 6-11 */
}

/* ---- install --------------------------------------------------------------------------------------------- */

void simple_dialogs_init(JSContext *ctx)
{
    static const IdlArgType ALERT_ARGS[1] = { IDL_DOMSTRING };
    static const IdlArgType CONFIRM_ARGS[1] = { IDL_DOMSTRING };
    static const IdlArgType PROMPT_ARGS[2] = { IDL_DOMSTRING, IDL_DOMSTRING };

    DCHECK(g_rt == NULL, "simple_dialogs_init ran twice — §8.9.1's members are declared once per agent");
    g_rt = JS_GetRuntime(ctx);
    /* `undefined alert(); undefined alert(DOMString message);` — one declaration for both entries, because
       §3.6 steps 3-4 separate two entries of different length by the ARGUMENT COUNT alone and the body reads
       that count back. No idl_arg_default here: see js_sd_alert for why `alert()` and `alert(undefined)` must
       stay two different calls. */
    g_id_alert = idl_method_id(ctx, ALERT_ARGS, 1, js_sd_alert, 0);
    idl_optional_from(0);
    /* `boolean confirm(optional DOMString message = "");` */
    g_id_confirm = idl_method_id(ctx, CONFIRM_ARGS, 1, js_sd_confirm, 0);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_STRING, "");   /* §3.6 steps 15.4.1 and 16.1's `= ""` */
    /* `DOMString? prompt(optional DOMString message = "", optional DOMString default = "");` */
    g_id_prompt = idl_method_id(ctx, PROMPT_ARGS, 2, js_sd_prompt, 0);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_STRING, "");
    idl_arg_default(1, IDL_DEFAULT_STRING, "");
    agent_state_ptr("simple_dialogs", &g_rt,
                    "the runtime HTML §8.9.1 Simple dialogs' three member declarations were registered in");
    agent_state_id("simple_dialogs", &g_id_alert, "HTML §8.9.1's `alert` declaration");
    agent_state_id("simple_dialogs", &g_id_confirm, "HTML §8.9.1's `confirm` declaration");
    agent_state_id("simple_dialogs", &g_id_prompt, "HTML §8.9.1's `prompt` declaration");
}

void simple_dialogs_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_rt != NULL, "HTML §8.9.1's members were installed before simple_dialogs_init declared them");
    idl_install_method(ctx, (JSValue)global, "alert", g_id_alert);
    idl_install_method(ctx, (JSValue)global, "confirm", g_id_confirm);
    idl_install_method(ctx, (JSValue)global, "prompt", g_id_prompt);
}

void simple_dialogs_free(void)
{
    DCHECK(g_rt != NULL, "HTML §8.9.1's members were released in an agent that never declared them");
    g_rt = NULL;
    g_id_alert = g_id_confirm = g_id_prompt = -1;
}
