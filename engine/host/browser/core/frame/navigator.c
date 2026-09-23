/* THE NAVIGATOR INTERFACE — HTML §8.10.1, Blink core/frame, the client-identity half of the browsing context.
 *
 * WHY IT MATTERS MORE THAN ITS SIZE SUGGESTS. `navigator` was absent, and a missing global is a THROW: a bundle
 * doing `navigator.userAgent.indexOf("Chrome")` aborted boot before reaching a single endpoint.
 *
 * IT IS AN INTERFACE, AND IT WAS NOT ONE. Every member below is `readonly attribute` on a PROTOTYPE, and they
 * were writable data properties on the instance — which is not a placement detail, it is four things a page
 * observes and real bundles do all four: `navigator.userAgent = "x"` stuck (a browser ignores it in sloppy mode
 * and throws in strict), `Object.getOwnPropertyNames(navigator)` listed every member (a browser lists none),
 * `delete navigator.userAgent` succeeded, and `Navigator.prototype` and `window.Navigator` did not exist at all
 * — so `navigator instanceof Navigator` threw and every UA sniffer that patches the prototype patched nothing.
 *
 * THE NAVIGATOR COMPATIBILITY MODE IS THE FACT THAT DECIDES HALF THIS FILE, and §8.10.1.1 says so: "The user
 * agent has a navigator compatibility mode, which is either Chrome, Gecko, or WebKit", and it "constrains the
 * NavigatorID mixin to the combinations of attribute values and presence of taintEnabled() and oscpu that are
 * known to be compatible with existing web content". THIS USER AGENT'S MODE IS CHROME. Stated once, it decides
 * four things that were otherwise four independent choices to get inconsistent: productSub is "20030107" (Gecko
 * would be "20100101"), vendor is "Google Inc." (Gecko the empty string, WebKit "Apple Computer, Inc."),
 * appVersion is the WHOLE trail after "Mozilla/" (Gecko truncates it at the first ";"), and `taintEnabled()`
 * and `oscpu` DO NOT EXIST — the spec puts them in a partial interface a user agent supports only "If the
 * navigator compatibility mode is Gecko". `taintEnabled` was installed here and is deleted with this line: a
 * Chrome-mode Navigator that answers it is a combination the sentence above exists to forbid, and Chrome itself
 * has not had it for years.
 *
 * THE OTHER DESIGN DECISION IS PER MEMBER, AND IT IS THE ONE CLAUDE.md STATES FOR matchMedia. Navigator's
 * members split cleanly in two, and getting the split wrong loses code either way:
 *
 *   SPEC-FIXED members are CONCRETE. HTML says `appCodeName` MUST return "Mozilla", `appName` MUST return
 *   "Netscape", `product` MUST return "Gecko", `vendorSub` MUST return the empty string, and javaEnabled()
 *   MUST return false. There is no other world for a branch to fork into, so a concolic here would model an
 *   ignorance the engine does not have and fork a branch whose sibling cannot exist. `productSub` and `vendor`
 *   are concrete for the same reason once the compatibility mode is stated: their alternatives are other
 *   BROWSERS, not other worlds this document could be in.
 *
 *   ENVIRONMENT members are CONCOLIC WITH A CONCRETE EXAMPLE. `userAgent`, `platform`, `language`, `onLine`,
 *   `hardwareConcurrency`, `maxTouchPoints` are exactly the values a bundle GATES ITS CODE ON — the mobile
 *   path, the Safari workaround, the locale-specific host, the offline queue — and every one of those gates
 *   hides endpoints this tool exists to find. Concretising them picks one arm and DELETES the others; that is
 *   the "a modelable value collapsed to bare-concrete deletes the fork and its coverage" failure. So each
 *   carries the value a real desktop Chrome would report as its EXAMPLE (so `ua.slice(0,4)` computes "Mozi"
 *   and a pinned comparison yields a real @H value) while staying opaque for control flow.
 *
 * WHAT IS HONESTLY ABSENT, and named by the IDL audit rather than by a comment that can go stale:
 * NavigatorPlugins' `plugins` and `mimeTypes` (§8.10.1.6's PluginArray/MimeTypeArray/Plugin/MimeType, four
 * interfaces this engine has not built) and NavigatorContentUtils' registerProtocolHandler /
 * unregisterProtocolHandler. A page reading one gets `undefined` from a member that does not exist, which is
 * the forcing function; a shape-only object with the right member names would be the stub the audit exists to
 * expose. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/cow.h"        /* the instance's record is a component's own C state — it time-travels */
#include "core/agent_state.h"
#include "core/file/storage_manager.h"
#include "core/frame/navigator.h"
#include "core/frame/navigator_beacon.h"
#include "core/html/user_activation.h"
#include "core/idl_args.h"
#include "core/permissions/permissions.h"
#include "core/realm.h"

/* A real desktop Chrome's identity, used as the EXAMPLE for the environment members. §8.10.1.1's appVersion
   getter steps are a SUBSTRING of this rather than a second string: "Let trail be the substring of userAgent
   that follows the `Mozilla/` prefix", and in Chrome mode the answer is that trail whole. Two constants that
   could disagree would be one fact answered from two places; the split is where the spec puts it. */
#define NAV_UA_PREFIX "Mozilla/"
#define NAV_UA_REST   "5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " \
                      "Chrome/131.0.0.0 Safari/537.36"
#define NAV_UA        NAV_UA_PREFIX NAV_UA_REST

/* THE MEMBER LIST, IN ONE PLACE, because it is read THREE times — the magic a getter carries is an index into
   it, the per-realm record is filled at those indices, and the install walks it to define the accessors. Three
   hand-kept lists is a member that exists in two of them and not the third, which is a getter answering
   undefined with nothing to say so; one X-list makes that unspellable, and the record's completeness check
   below is the other half of the assertion.
   `userActivation` is NOT here: it is the one member whose value is not a fact stored for the realm but §6.4.1
   state read at the moment of the call, so it has its own getter. */
/* THE THIRD COLUMN IS WEB IDL §3.9's EXPOSURE, and it is a column rather than a branch for the same reason the
   other two are: a member's IDL states it, so it is DATA about the member and the install reads it off the one
   list. `NavigatorDeviceMemory` is the mixin that carries [SecureContext] — the attribute is on the MIXIN, so
   every member of it inherits the condition — and Chrome accordingly has no `deviceMemory` property on
   Navigator.prototype over `http`. That is not a value difference a page shrugs at: `if (navigator.deviceMemory
   > 4)` and `'deviceMemory' in navigator` both take the other road, and whatever is down that road is code
   this engine would otherwise never run. */
#define NAV_MEMBERS(X)                                                             \
    /* NavigatorID — §8.10.1.1 */                                                  \
    X(APP_CODE_NAME,         "appCodeName",          IDL_EXPOSED)                  \
    X(APP_NAME,              "appName",              IDL_EXPOSED)                  \
    X(APP_VERSION,           "appVersion",           IDL_EXPOSED)                  \
    X(PLATFORM,              "platform",             IDL_EXPOSED)                  \
    X(PRODUCT,               "product",              IDL_EXPOSED)                  \
    X(PRODUCT_SUB,           "productSub",           IDL_EXPOSED)                  \
    X(USER_AGENT,            "userAgent",            IDL_EXPOSED)                  \
    X(VENDOR,                "vendor",               IDL_EXPOSED)                  \
    X(VENDOR_SUB,            "vendorSub",            IDL_EXPOSED)                  \
    /* NavigatorLanguage — §8.10.1.2 */                                            \
    X(LANGUAGE,              "language",             IDL_EXPOSED)                  \
    X(LANGUAGES,             "languages",            IDL_EXPOSED)                  \
    /* NavigatorOnLine — §8.10.1.3 */                                              \
    X(ON_LINE,               "onLine",               IDL_EXPOSED)                  \
    /* NavigatorCookies — §8.10.1.5 */                                             \
    X(COOKIE_ENABLED,        "cookieEnabled",        IDL_EXPOSED)                  \
    /* NavigatorPlugins — §8.10.1.6 */                                             \
    X(PDF_VIEWER_ENABLED,    "pdfViewerEnabled",     IDL_EXPOSED)                  \
    /* NavigatorConcurrentHardware — HTML §10 */                                   \
    X(HARDWARE_CONCURRENCY,  "hardwareConcurrency",  IDL_EXPOSED)                  \
    /* NavigatorAutomationInformation — WebDriver §12 */                           \
    X(WEBDRIVER,             "webdriver",            IDL_EXPOSED)                  \
    /* NavigatorDeviceMemory — Device Memory §3, and the mixin is `[SecureContext]` */ \
    X(DEVICE_MEMORY,         "deviceMemory",         IDL_SECURE_CONTEXT)           \
    /* partial interface Navigator — Pointer Events 4 §6 "Extensions to the Navigator interface".        \
       THE NAME, THE NUMBER AND THE QUOTED TITLE ARE ON ONE LINE ON PURPOSE: a resolver reads a          \
       standard's name from the words before the section sign and a title from the quoted run after      \
       it, so a wrap between any two of them separates the citation from its anchor.                     \
       THIS SAID `§12`, AND THAT SECTION IS "Wheel Events and interfaces" — it owns no member of         \
       Navigator at all. §6 is where `maxTouchPoints` carries its own dfn and its own getter steps.      \
       NOTHING HERE COULD HAVE CAUGHT IT AND NOTHING HERE EVER WILL. Pointer Events has no committed     \
       corpus row and cannot get one: the edition its editors maintain is unrendered ReSpec source,      \
       which carries a section number on no heading of either shape, so there is nothing to index.       \
       It is a FOREIGN entry — counted, and openly never checked — and this line is what that band       \
       costs. The other six numbers this tree writes for this standard were re-derived against the       \
       rendered edition at the same time and are right; this was the one that was not, which is why      \
       a band nothing judges is read entry by entry rather than trusted for being quiet. */              \
    X(MAX_TOUCH_POINTS,      "maxTouchPoints",       IDL_EXPOSED)

#define NAV_ENUM_ONE(id, str, exposure) NAV_##id,
#define NAV_NAME_ONE(id, str, exposure) str,
#define NAV_EXPOSURE_ONE(id, str, exposure) exposure,

enum { NAV_MEMBERS(NAV_ENUM_ONE) NAV_N };
static const char *const NAV_NAME[] = { NAV_MEMBERS(NAV_NAME_ONE) };
static const IdlExposure NAV_EXPOSURE[] = { NAV_MEMBERS(NAV_EXPOSURE_ONE) };

/* THE TWO MEMBERS THIS MODE FORBIDS. §8.10.1.1 puts them in a partial interface the user agent supports only
   "if the navigator compatibility mode is Gecko", and the published html.idl carries that partial FLAT — the
   condition is a sentence, not an extended attribute. So an audit of this file against the corpus sees two
   members nobody installed and cannot tell "this UA must not have them" from "nobody has written them yet",
   which is how a member the spec FORBIDS here gets added by someone working an ABSENT list. Stated here, beside
   the mode the four lines above commit to, and asserted per realm by idl_members_excluded. */
static const char *const NAV_MODE_EXCLUDED[] = { "taintEnabled", "oscpu" };

/* `clipboard` IS AN ABSENT ROW THIS FILE DELIBERATELY LEAVES STANDING, AND IT IS NOT A CANDIDATE FOR THE
   DECLARATION ABOVE. The two members up are ones the SPEC FORBIDS this user agent; this is one the engine
   OWES and has not built, so an idl_members_excluded entry for it would state something about the Clipboard
   API that the Clipboard API does not say, and would spend the only row the work has. The audit carries
   `clipboard` inside this interface's ABSENT list and carries NO row at all for `Clipboard` or
   `ClipboardItem`, because an auditor keyed on what EXISTS cannot count an interface that is wholly absent.

   WHY IT IS NOT LANDED AS A HALF, WHICH IS THE QUESTION A READER ARRIVES WITH. The member is cheap to
   picture and its guard population is not: the page-facing unit is TWO interfaces, and real bundles guard
   them as a CONJUNCTION IN BOTH ORDERS, so either alone turns a guard that works today into a throw.
     - This member WITHOUT `ClipboardItem`: a site writing
       `navigator.clipboard?.write([new ClipboardItem({...})])` evaluates no argument at all today, because
       ECMAScript §13.3.9.1 "Runtime Semantics: Evaluation" short-circuits an optional chain on an undefined
       BASE and never reaches what follows it. The moment this member exists the chain proceeds, the argument
       is evaluated, and `ClipboardItem` — which platform_names.h lists, so absent.c leaves that read alone
       instead of forking it — raises a ReferenceError at a line that costs nothing now.
     - `ClipboardItem` WITHOUT this member: a site writing
       `typeof ClipboardItem != "undefined" && navigator.clipboard.write` short-circuits on the FIRST
       conjunct today and dereferences `navigator.clipboard` UNGUARDED the moment that conjunct turns true.
   Neither is the smaller half of one landing; they are two halves of one, and CLAUDE.md §NO-STUBS' unit —
   the smallest diff that makes the guard's TRUE branch survivable — is both interfaces or neither.

   AND THE OPTIONAL-CHAIN POPULATION IS NOT THE THROWING POPULATION, which is the reading this shape invites
   and which the sites refute. An optional chain guards the BASE, so what an absent member costs depends on
   what the site does with it: a non-optional CALL through the chain throws, an optional call short-circuits,
   and a member read as a VALUE — a ternary condition, a `&&` operand, a render test — is merely falsy.
   Counting the optional chain on the attribute counts all three and reports the last two as hazards they are
   not. The property worth counting is a NON-OPTIONAL CALL of the member.

   THE DERIVATION, never a figure, because a corpus moves and this one grew by three quarters while the
   question was being asked. engine/absentrank.mjs already publishes the receiver spellings and their armed
   control; what this adds is the CALL-versus-VALUE partition, over a mirror of real bundles, with an
   invented member as the negative control and the ClipboardItem neighbourhood READ rather than counted.
   The patterns are kept SHORT on purpose: a long run in quotes is read as a quotation belonging to the
   nearest citation above it, which is a finding this block earned once before it was written this way.
     cd <dir>/mirror
     grep -rohE 'navigator\.clipboard' . | wc -l
     grep -rohE 'clipboard\?\.(read|readText|write|writeText)\(' . | sort | uniq -c
     grep -rohE 'clipboard\?\.zzznope\(' . | wc -l
     grep -rohE '.{110}ClipboardItem.{60}' .

   WHAT IS NOT COVERED: this Navigator declares no `clipboard` member, so Clipboard API §7.1.1 "clipboard"'s
   getter steps never run, no Clipboard object is minted in any realm, and none of Clipboard API §7.3
   "Clipboard Interface"'s four operations exists. That is CORRECT and NARROWER rather than unfinished: a
   real browser without the Async Clipboard API answers this read with the undefined that
   ECMAScript §10.1.8.1 "OrdinaryGet ( obj, propertyKey, receiver )" step 2.b returns, which is what every
   guard in the corpus is written against and what this engine already gives.
     THE READ HALF IS NARROWER FOR A SECOND REASON, WHICH IS THE STANDARD'S OWN AND SURVIVES THE BUILD.
   Clipboard API §9.1.1 "check clipboard read permission" consults no permission descriptor, reads no
   permission store and requests nothing; its only true arm is a script running from a Paste element created
   by the user agent or the operating system, and it returns false otherwise. Clipboard API §9 "Permissions
   API Integration" states the same thing directly, that one permission is defined for the clipboard and it
   is the write one — which is the sentence core/permissions/permission_store.c reasoned to when it
   registered that feature and refused a read one, reached from the specification rather than from Chrome. So
   a built readText() would REJECT in every world this engine can model, and that rejection is the answer
   Clipboard API §7.3.2 "readText()" computes rather than a stub.
     THE WRITE HALF IS THE OPPOSITE, AND IS THE HALF THE READ REFUSAL DOES NOT REACH. Clipboard API §9.2.1
   "check clipboard write permission" reaches a permission request on BOTH of its arms, which
   permission_request_run already answers by FORKING the user's decision, over a feature the store already
   carries with its aspect and its partial order. What forbids a write-only landing is the entanglement
   above, never this check.

   WHAT THE NEXT DIFF BUILDS, AS ONE LANDING — numbered as one and not as five, because a set that cannot
   land except together is a single landing: Clipboard API §7.2 "ClipboardItem Interface" whole, whose
   constructor, presentationStyle, types, getType and static supports round-trip inside the object and need
   no system clipboard at all; §7.3's four operations as STEP MACHINES, each parking across a permission
   question; §7.1 "Navigator Interface"'s [SecureContext, SameObject] attribute here, as IDL_SECURE_CONTEXT
   and reading the receiver's environment exactly as `permissions` does below; Clipboard API §4 "Model"'s
   system clipboard as per-agent state that ROUND-TRIPS, so a page's own write is its read's reader; and
   §9.2.1's check over permission_request_run. Every mechanism this clause names was grepped before it was
   written — permission_request_run in core/permissions/permission_store.h, transient activation in
   core/html/user_activation.c, Blob in core/file/blob.c — and ClipboardItem was grepped too and is in the
   generated name tables and in no component.
     WHAT IS NOT IN THAT LANDING AND SITS BEFORE IT IN THE STANDARD'S OWN ORDER. Clipboard API §7.3.3
   "write(data)" reads the mandatory and optional data type lists that Clipboard API §6 "Clipboard Event API"
   owns, and both permission checks turn on user interaction with a user-agent-created affordance, which is
   Clipboard API §8 "Clipboard Actions"' and §5.3 "Integration with other scripts and events"'. DataTransfer,
   ClipboardEvent and the copy, cut and paste actions are in the generated name tables and in no component,
   so those arms are false BY ABSENCE rather than by decision — sound for a user agent offering no such
   affordance, and the reason a read() sitting inside a paste handler, which is how the corpus reaches it,
   would be unreachable here even with §7 built.

   HOW ITS ABSENCE WOULD SHOW: the audit's Navigator line carries `clipboard` among its ABSENT members while
   no line anywhere names Clipboard or ClipboardItem, and a page whose copy control is guarded reports its
   fallback path rather than its clipboard one. NO INSTRUMENT HERE JUDGES THE CITATIONS ABOVE: `clipboard`
   and `clipboard api` are both foreign entries in engine/citegen.mjs, so this block is COUNTED and never
   CHECKED, which is what that band costs and is why each number was fetched one at a time from the editor's
   draft this standard's own editors maintain. That draft's heading list is identical to the published
   snapshot's, so no level and no edition separates them.
   RETIREMENT: this block goes when both interfaces are installed, at which point the audit's Navigator line
   stops naming `clipboard` and the rows that replace it are what a reader should be reading instead. */

/* THE CLASS IS THE BRAND. Web IDL §3.7.6 "Attributes" makes every getter refuse a receiver that "does not
   implement the interface" before it reads anything, and §3.7.7 "Operations" says the same of every method —
   and the one object per realm WEARS the class, so the check is a class-id comparison a page cannot forge. It
   carries no per-object data — the values are the realm's — so it needs no finalizer and no gc_mark.
   THE NUMBER USED TO BE §3.7.5 HERE AND IN THREE OTHER PLACES, and §3.7.5 is "Constants": a citation that
   sends the reader to a section saying nothing about receivers reads as authority and is checkable only by
   someone who fetches the text, which is why a WRONG number is worse than none. */
static JSClassID g_nav_class;
static JSClassID g_obj_slot = JS_INVALID_CLASS_ID;   /* this realm's one Navigator — `window.navigator`'s [SameObject] holder */

/* THE RECORD — the environment §8.10.1's members answer from, and the member values, carried by the Navigator
 * rather than by the realm.
 *
 * WHY THE INSTANCE AND NOT THE REALM. A C member runs in the realm that DEFINED it (js_call_c_function does
 * `ctx = p->u.cfunc.realm`), so a member reading a per-REALM slot answers out of whichever realm's prototype
 * the call went through, not out of the receiver. HTML §6.4.4 "The UserActivation interface" says "the
 * userActivation getter steps are to return THIS's relevant global object's associated UserActivation", and
 * PERMISSIONS §6.1's member is the same shape — the input is the receiver's global, and a realm slot is
 * structurally unable to supply it.
 *
 * THE ASSERT THAT STOOD HERE WAS A PAGE-HELD ABORT SWITCH. It compared the receiver against this realm's own
 * Navigator and DCHECKed them equal — and a receiver is PAGE-SUPPLIED INPUT, which a DCHECK may never stand on
 * (CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE). `Object.getOwnPropertyDescriptor(Navigator.prototype, "userAgent")
 * .get.call(otherFrame.navigator)` is two lines of ordinary JavaScript and ended the process, and a FORCING
 * solver writes receivers like that constantly. What Web IDL §3.7.6/§3.7.7 ask is the BRAND and nothing
 * beside it; `nav_brand` already answers that with a TypeError, so the realm comparison was a different
 * question with no standing rather than a stricter version of the same one.
 *
 * TWO FIELDS FOR TWO FACTS. `vals` is what the X-list members answer with — a per-realm array, so it has to
 * ride the object that names the realm. `realm` is the ENGINE's handle on the environment, for the two members
 * whose value belongs to ANOTHER component keyed by realm (user_activation.c's and permissions.c's): those
 * answer state read at the call rather than a stored value, so what they need is the receiver's context.
 *
 * `global` IS WHAT MAKES `realm` SAFE, and it is a declared JSValue edge rather than a `JS_DupContext`. A raw
 * JSContext in a record whose object can outlive its realm is a dangling handle — a page can hold
 * `otherFrame.navigator` after the frame is gone — and holding the realm's GLOBAL closes it, because the
 * global's members are C function objects each holding a counted reference to the realm that defined them, so
 * a live global is a live realm. A context reference hung off an opaque would be invisible to gc_decref and
 * would make the realm PERMANENTLY uncollectable; a JSValue in the layout below is marked, freed and dup'd like
 * any other, so the collector can still break the cycle. core/timing/performance.c states this same reasoning
 * at its own record and is where it was read from.
 *
 * EVERY CROSS-REALM RECEIVER THAT CAN REACH THIS IS SAME-AGENT, so `realm` is always a context of this runtime:
 * SECURITY.md keys an instance on `(browsing context group, origin)`, and HTML §7.2.1.3.1's
 * CrossOriginProperties(Window) does not carry `Navigator`, so a cross-ORIGIN document's interface object is
 * not reachable as a JS object at all. What IS reachable is a same-origin document of this agent, which is one
 * heap. */
typedef struct {
    JSContext *realm;    /* the environment the two component-owned members answer from. NOT a counted ref */
    JSValue    global;   /* "this's relevant global object" — OWNED, and what holds `realm` up */
    JSValue    vals;     /* §8.10.1's member values, indexed by the enum above. OWNED. */
} Navigator;

/* THE ONE STATEMENT OF WHAT THE RECORD OWNS — the same list the finalizer frees and the gc_mark walks, which
   is why all three are written here together. `realm` is not in it: it is a pointer and not a JSValue, so the
   capture copies its BYTES with the rest of the record and never dups or frees it, which is exactly right for
   a handle whose lifetime the value beside it guarantees. */
static const uint16_t NAV_VAL_OFF[] = {
    (uint16_t)offsetof(Navigator, global),
    (uint16_t)offsetof(Navigator, vals),
};
static const CowRecord NAV_REC = { sizeof(Navigator), NAV_VAL_OFF, 2 };
static int g_id_java_enabled = -1;

/* WEB IDL §3.7.6 "Attributes"' BRAND CHECK. `Navigator.prototype.userAgent` read off a plain object is a TypeError, and a
   page tells that apart from `undefined` — a feature detector that probes the descriptor and applies the getter
   reads the throw as "this is a real interface". It is a real throw and not an assert for exactly that reason. */
static bool nav_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_nav_class != 0, "a Navigator member ran before navigator_init declared the class — the member is "
                             "only reachable through a prototype the per-realm install builds, so there is no "
                             "route here that has not run the declaration first");
    if (JS_GetClassID(this_val) == g_nav_class) return true;
    JS_ThrowTypeError(ctx, "a Navigator member was reached on something that is not a Navigator");
    return false;
}

/* THE SAME BRAND, ASKED FROM OUTSIDE — see navigator.h. It is the class comparison and nothing else: the
   THROW belongs to the member, because a partial's member throws its own message and some of them are not
   plain getters at all. */
bool navigator_is(JSValueConst v)
{
    DCHECK(g_nav_class != 0, "Web IDL §3.7.6/§3.7.7's brand was asked before navigator_init declared the "
                             "class — a "
                             "partial interface's member is only reachable through a prototype this "
                             "component's per-realm install builds, so there is no route here first");
    return JS_GetClassID(v) == g_nav_class;
}

/* THE ACCESSOR EVERY MEMBER REACHES THE RECORD THROUGH, and the capture is IN it for solver/cow.h's reason: a
   record a flow has REACHED is one it may write, the delta dedups to one entry per (flow, object), and there is
   then no write site left to miss. Bounded by §8.10.1's own shape — one Navigator per realm, so at most one
   delta entry per realm per flow.
   NOT nav_brand and NOT navigator_is: a brand check is a QUESTION, asked of values that are not Navigators at
   all, and a question must not capture. */
static Navigator *nav_rec(JSValueConst v)
{
    Navigator *n = g_nav_class ? JS_GetOpaque(v, g_nav_class) : NULL;

    if (n) cow_capture_host_record(v, n, &NAV_REC);
    return n;
}

/* JS_GetAnyOpaque and not JS_GetOpaque in BOTH of these — core/agent_state.h's rule: the collector dispatched
   here THROUGH the class, so the id is a fact it already has and must not look up. */
static void nav_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    Navigator *n = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(n != NULL, "a Navigator was finalized with no record — §8.10.1's object has exactly one mint and it "
                      "attaches the record with nothing in between that could collect");
    JS_FreeValueRT(rt, n->global);
    JS_FreeValueRT(rt, n->vals);
    free(n);
}

static void nav_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    Navigator *n = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(n != NULL, "a Navigator was marked with no record — its global and its member array are counted "
                      "references, and an unmarked child keeps the internal count gc_decref subtracts, so "
                      "gc_scan reads it as rooted from OUTSIDE the heap and it is never collected at all");
    JS_MarkValue(rt, n->global, mark_func);
    JS_MarkValue(rt, n->vals, mark_func);
}

/* "THIS's RELEVANT GLOBAL OBJECT", ANSWERED RATHER THAN ASSERTED ABOUT — the environment §8.10.1's members
   name, taken off the RECEIVER. Every assert here is about a value THIS component wrote at the mint, which is
   the only thing a DCHECK may stand on; the receiver itself is page-supplied and its brand is a TypeError one
   line above every caller.
   EXPORTED, because a member another STANDARD puts on Navigator needs the same environment and cannot reach
   this record: Storage §8's `storage` is the first such caller. A caller asks the BRAND first — this asserts
   rather than refuses, so it may only be reached with a value `navigator_is` has already answered true for. */
JSContext *navigator_environment(JSValueConst this_val)
{
    Navigator *n = nav_rec(this_val);

    DCHECK(n != NULL, "a Navigator reached a member with no record — the brand is the class and the mint "
                      "attaches the record before the object leaves it, so a branded object without one came "
                      "from a second mint that does not exist");
    DCHECK(n->realm != NULL, "a Navigator names no environment — §8.10.1's mint is the one writer of this "
                             "field and it writes the realm it is installing into");
    return n->realm;
}

/* THE VALUE A MEMBER ANSWERS WITH, out of THE RECEIVER'S record. Owned — the caller returns it. */
static JSValue nav_value(JSValueConst this_val, int idx)
{
    Navigator *n = nav_rec(this_val);
    JSValue v;

    DCHECK(idx >= 0 && idx < NAV_N, "a Navigator getter was installed with a magic that is not a member index "
                                    "— the magic IS the index into the one member X-list");
    DCHECK(n != NULL, "a Navigator reached a member with no record — see nav_environment");
    v = JS_GetPropertyUint32(n->realm, n->vals, (uint32_t)idx);
    DCHECK(!JS_IsUndefined(v), "a Navigator member's record holds nothing at its index — the member list and "
                               "the record builder are one X-list, so an empty index means a member was "
                               "declared and never given the value its IDL says it answers with");
    return v;
}

/* EVERY DECLARED MEMBER'S GETTER, once. Its magic is its index; there is nothing per member to write, which is
   what stops a member from arriving with a hand-written getter that forgets the brand check. */
static JSValue js_nav_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    return nav_value(this_val, magic);
}

/* HTML §8.10.1.6: "The NavigatorPlugins mixin's javaEnabled() method steps are to return false." A no-effect
   would be a stub; returning the value the spec states is the implementation. */
static JSValue js_nav_java_enabled(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    (void)argc; (void)argv; (void)magic;
    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    return JS_FALSE;
}

/* HTML §6.4.4: `partial interface Navigator { [SameObject] readonly attribute UserActivation userActivation; }`.
   "The userActivation getter steps are to return this's relevant global object's associated UserActivation" —
   which user_activation.c minted with this realm, so the SAME object comes back on every read without this
   getter caching anything, and the two booleans behind it are §6.4.1's real state rather than a constant. It is
   not in the record because it is not a value stored for the realm: it is state read at the call. */
static JSValue js_nav_user_activation(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    /* THE ENVIRONMENT IS THE RECEIVER'S AND NOT `ctx` — §6.4.4 says "this's relevant global object's
       associated UserActivation", so the argument is what makes this member §6.4.4 rather than one that
       reports whichever document's interaction the page reached the getter through. */
    return user_activation_object(navigator_environment(this_val));
}

/* PERMISSIONS §6.1: `partial interface Navigator { [SameObject] readonly attribute Permissions permissions; }`,
   installed here for the reason `userActivation` is — the ATTRIBUTE belongs on §3.7's interface prototype
   object, which is this component's, while the VALUE is the permissions component's one object for the realm.
   [SameObject] therefore comes from where that object is KEPT and not from a cache in this getter. */
static JSValue js_nav_permissions(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    /* THE ENVIRONMENT IS THE RECEIVER'S AND NOT `ctx`, for §6.4.4's reason one member up: [SameObject] comes
       from where the permissions component KEEPS its object, and which object that is depends on whose realm
       is asking. */
    return permissions_object(navigator_environment(this_val));
}

/* HTML §7.2.2 "The Window object"'s two Window members that name this object — that is where the IDL sits, and
   HTML §8.10.1 "The Navigator object" is where their steps do: "The navigator and clientInformation getter
   steps are to return this's associated Navigator." (§7.2.5 stood here for both and is "The History
   interface".) `navigator` is a plain `readonly attribute Navigator`
   and `clientInformation` is `[Replaceable] readonly attribute Navigator` — the legacy alias, which the IDL
   marks replaceable and the real one does not, so they install through different helpers. BOTH read the ONE
   realm slot rather than one of them holding a second reference: `navigator === clientInformation` is what HTML
   means by "legacy alias of .navigator", and two stored references are two things that can come apart. */
static JSValue js_win_navigator(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return realm_value_get(ctx, g_obj_slot);   /* OWNED — realm_value_get asserts the realm ran its install */
}

/* THIS REALM'S Navigator, FOR A MEMBER ANOTHER STANDARD PUTS ON IT — AND A CALLER ASKS WEB IDL §3.8 "Platform
   objects implementing interfaces" STEP 1 OF `Navigator` BEFORE CALLING, because this realm may not have one.
   The install below refuses outright where it does not, so the slot this reads is unset in a worker realm and
   core/realm.c's realm_value_get aborts: there is no soft answer here and there must not be one.
   TWO CITATIONS THAT STOOD HERE WERE WRONG AND ARE CORRECTED RATHER THAN DROPPED, because a reader who
   re-derives them writes them again. The member was given as `Storage §2's navigator.storage declared on
   partial interface Navigator`: Storage §2 is "Terminology" and declares nothing, the member is Storage §8
   "API"'s, and Storage §8 declares no partial interface at all — it declares `interface mixin
   NavigatorStorage` with an `includes` statement for `Navigator`, which Web IDL §3.7.3 "Interface prototype
   object" treats differently from a partial interface in the one way that matters here: a mixin has no
   prototype object of its own, so its member has no object until an includer supplies one. The page-facing
   member was given as HTML §7.2.5's, which is "The History interface"; `Window`'s `navigator` and
   `clientInformation` are declared in HTML §7.2.2 "The Window object" and this object is HTML §8.10.1 "The
   Navigator object"'s. OWNED. */
JSValue navigator_object(JSContext *ctx)
{
    return realm_value_get(ctx, g_obj_slot);
}

/* ---- the per-realm record ------------------------------------------------------------------------------- */

static void nav_put(JSContext *ctx, JSValueConst rec, int idx, JSValue v)
{
    DCHECK(idx >= 0 && idx < NAV_N, "a Navigator member value was recorded at an index that is not a member");
    CHECK(!JS_IsException(v), "a Navigator member's value could not be allocated");
    JS_SetPropertyUint32(ctx, rec, (uint32_t)idx, v);
}

/* An ENVIRONMENT member: opaque for control flow, carrying what a real browser would answer. One helper so a
   later member cannot accidentally be added as bare-concrete — a Navigator member IS its own source, so the
   two halves are spelled from ONE token here.
   THE SHAPE IS THAT TOKEN IN BRACES AND THE SOURCE IDENTITY IS IT BARE, which is the rule concolic_new
   asserts and which this file used to break by passing one string as both. The consequence was not a
   cosmetic one: `navigator.userAgent`, `navigator.maxTouchPoints` and `navigator.hardwareConcurrency` are
   three of the most-branched-on values a real bundle has, and with no brace in the shape concolic_hole_key
   answered NULL for every one of them — so every gate over this interface was observed and then dropped, and
   the endpoints behind those gates reported their parameters with no domain at all. */
static void nav_env(JSContext *ctx, JSValueConst rec, int idx, JSValue example)
{
    char path[64], hole[66];
    JSValue v;

    DCHECK(idx >= 0 && idx < NAV_N, "a Navigator environment value was minted for a non-member index");
    DCHECK(strlen(NAV_NAME[idx]) + 11 < sizeof(path), "a Navigator member name longer than any in the IDL");
    snprintf(path, sizeof(path), "navigator.%s", NAV_NAME[idx]);
    snprintf(hole, sizeof(hole), "{%s}", path);
    /* THE SEAM, and not concolic_new. §CLAUDE splits the halves exactly here: the browser computes what the
       spec says the member is, and the SOLVER decides it is also symbolic — so a host that installs the value
       semantics and NOT the source overlay (the conformance runner does exactly that) gets the plain value
       back. Minting directly made every environment member of this interface answer an OBJECT there, so
       `typeof navigator.userAgent` was "object" in the one host whose whole job is measuring fidelity. */
    v = concolic_source_wrap(ctx, hole, path, example);
    CHECK(!JS_IsException(v), "minting a Navigator environment value failed");
    JS_SetPropertyUint32(ctx, rec, (uint32_t)idx, v);
}

/* THIS REALM'S MEMBER VALUES, built with the realm. Written here and not lazily on a first read for the reason
   §3.7 makes every prototype per realm: a value built on first touch is built inside whichever flow happened to
   ask first, and that flow's baseline becomes every other flow's. Returns an OWNED array; the caller hands it
   to the realm slot. */
static JSValue nav_build_values(JSContext *ctx)
{
    JSValue rec = JS_NewArray(ctx), langs, lang;
    int i;

    CHECK(!JS_IsException(rec), "the Navigator member record could not be allocated");

    /* ---- SPEC-FIXED: concrete, because the spec admits no other answer ---- */
    nav_put(ctx, rec, NAV_APP_CODE_NAME, JS_NewString(ctx, "Mozilla"));
    nav_put(ctx, rec, NAV_APP_NAME,      JS_NewString(ctx, "Netscape"));
    nav_put(ctx, rec, NAV_PRODUCT,       JS_NewString(ctx, "Gecko"));
    nav_put(ctx, rec, NAV_VENDOR_SUB,    JS_NewString(ctx, ""));
    /* Chrome compatibility mode, per the file comment: these two ARE the mode, read off §8.10.1.1's lists. */
    nav_put(ctx, rec, NAV_PRODUCT_SUB,   JS_NewString(ctx, "20030107"));
    nav_put(ctx, rec, NAV_VENDOR,        JS_NewString(ctx, "Google Inc."));

    /* ---- ENVIRONMENT: concolic, example = what a real Chrome answers ---- */
    /* §8.10.1.1's appVersion steps return the empty string unless the user agent starts with `Mozilla/5.0 (`,
       and the trail after `Mozilla/` otherwise. The example below IS that trail, so the modelled user agent has
       to satisfy the test the steps make of it — asserted here rather than left to whoever edits the string. */
    DCHECK(strncmp(NAV_UA, "Mozilla/5.0 (", 13) == 0,
           "§8.10.1.1's appVersion getter returns the EMPTY STRING for a user agent that does not start with "
           "`Mozilla/5.0 (`, and this file's example is the trail after `Mozilla/` — so a modelled user agent "
           "that fails the test would make appVersion answer a substring the spec says is not its value");
    nav_env(ctx, rec, NAV_USER_AGENT,  JS_NewString(ctx, NAV_UA));
    nav_env(ctx, rec, NAV_APP_VERSION, JS_NewString(ctx, NAV_UA_REST));
    nav_env(ctx, rec, NAV_PLATFORM,    JS_NewString(ctx, "Win32"));
    nav_env(ctx, rec, NAV_ON_LINE,     JS_TRUE);
    nav_env(ctx, rec, NAV_COOKIE_ENABLED,    JS_TRUE);
    nav_env(ctx, rec, NAV_PDF_VIEWER_ENABLED, JS_TRUE);
    /* WebDriver: a page's anti-automation gate. The engine IS automation, but what a page can observe is the
       flag a browser sets, and BOTH answers lead to code worth reaching — which is exactly why it forks. */
    nav_env(ctx, rec, NAV_WEBDRIVER,   JS_FALSE);
    /* `unsigned long long` and `double` respectively, which is the only thing their two IDLs differ on here. */
    nav_env(ctx, rec, NAV_HARDWARE_CONCURRENCY, JS_NewInt32(ctx, 8));
    nav_env(ctx, rec, NAV_DEVICE_MEMORY,        JS_NewFloat64(ctx, 8));
    /* Pointer Events. `maxTouchPoints === 0` is the desktop-vs-touch gate a responsive bundle routes on, and
       the two arms ship different code. */
    nav_env(ctx, rec, NAV_MAX_TOUCH_POINTS,     JS_NewInt32(ctx, 0));

    /* §8.10.1.2's two members are ONE FACT: "The most preferred language is the one returned by
       navigator.language", so `languages[0]` is `language` — the SAME concolic value object, not a second one
       minted from the same string. Two would compare unequal under `===` where a browser compares two equal
       strings, and a flow pinning one would leave the other unpinned. */
    nav_env(ctx, rec, NAV_LANGUAGE, JS_NewString(ctx, "en-US"));
    /* Read back out of the record being built, not through nav_value: the realm slot this record is going into
       is not set until this function RETURNS it, and reading a slot before its install is what realm.h's own
       assert is for. */
    lang = JS_GetPropertyUint32(ctx, rec, NAV_LANGUAGE);
    CHECK(!JS_IsException(lang), "navigator.language could not be read back out of the record it was just "
                                 "written into");
    /* `FrozenArray<DOMString>`, so a real Web IDL frozen array rather than a scalar — a bundle writes
       `navigator.languages.includes("de")` and `languages[0]`, and both must work. §8.10.1.2 also says "The
       same object must be returned until the user agent needs to return different values", which is why it
       lives in the realm's record like every other value rather than being rebuilt per read. */
    langs = JS_NewArray(ctx);
    CHECK(!JS_IsException(langs), "the navigator.languages allocation failed");
    JS_SetPropertyUint32(ctx, langs, 0, lang);
    CHECK(idl_freeze_array(ctx, langs) == 0, "navigator.languages could not be frozen");
    nav_put(ctx, rec, NAV_LANGUAGES, langs);

    /* THE OTHER HALF OF THE X-LIST'S ASSERTION: every declared member got a value. A member added to the list
       and not to the builder is a getter that answers undefined, and this is where that is caught rather than
       in whichever bundle happens to read it. */
    for (i = 0; i < NAV_N; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, rec, (uint32_t)i);
        bool got = !JS_IsUndefined(v);
        JS_FreeValue(ctx, v);
        DCHECK(got, "a Navigator member was declared in the X-list and given no value by the record builder");
    }
    return rec;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

/* ONE PROTOTYPE, ONE INTERFACE OBJECT AND ONE NAVIGATOR PER REALM, built WITH the realm. HTML gives every
   Window an associated Navigator, and §3.7 gives every realm its own interface prototype object — and here
   that is not identity pedantry: js_call_c_function resolves `ctx` from the function object, so a prototype
   built once would answer every document's `navigator.userActivation` out of whichever realm built it. */
static void navigator_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, nav;
    Navigator *n;
    int i;

    /* WEB IDL §3.8 "Platform objects implementing interfaces"' internally create a new object implementing the
       interface, STEP 1: "Assert: interface is exposed in realm." — asked of the ONE generated table that
       states it, the way core/workers/worker_global_scope.c asks §3.8's other exposure step at the head of its
       own install. `Navigator` is `[Exposed=Window]`, so a realm whose §3.3.8 [Global] interface is a
       WorkerGlobalScope has nothing to build here: not the instance step 1 names, not §3.7.3's prototype and
       not §3.7.1's interface object.
       IT IS A REFUSAL AND NOT A MISPLACEMENT. HTML §10.2.1.1 "The WorkerGlobalScope common interface" gives a
       worker realm its OWN `readonly attribute WorkerNavigator navigator` — a DIFFERENT member of a DIFFERENT
       type, which this build does not have — so moving THIS object onto WorkerGlobalScope.prototype would put
       an interface the realm does not expose into it. Until WorkerNavigator exists the member is honestly
       ABSENT (§NO STUBS).
       THE NAMED RESIDUAL THAT STOOD HERE IS RETIRED, AND SO IS THE GATE THAT RETIRED IT. Its remedy clause
       asked for core/file/storage_manager.c to ask this same Web IDL §3.8 step 1 over `Navigator`, and that
       landed and has since been DELETED — the clause was right that the question belongs to the INCLUDER and
       one step short about where: this refusal IS the includer's answer, and it returns before there is a
       §3.7.3 prototype to hand anybody, so that component is now CALLED from below rather than gated. There
       is no realm in which it can be reached and owe nothing, which is why the state a gate would look for no
       longer exists. The clause is kept as a correction because two of the things it told its reader were
       wrong in ways the next reader would reproduce.
       (a) IT NAMED THE WRONG CONSTRUCT. It said Storage §8 declares `partial interface Navigator` AND
       `partial interface WorkerNavigator`; the harvested IDL every instrument here consumes declares
       `interface mixin NavigatorStorage` and two `includes` statements. Web IDL §3.7.3 "Interface prototype
       object" gives a mixin no prototype of its own, so a mixin member has no object ANYWHERE until an includer
       supplies one — which is the reason the gate is over the includer, and a `partial interface` would not
       have had it.
       (b) ITS ABSENCE-SHOWS CLAUSE NAMED AN ABORT THAT COULD NOT FIRE. It said a worker realm getting past
       this row aborts at a DCHECK complaining the realm has no Navigator. That DCHECK tested a value
       `navigator_object` produces, and reading a realm slot this install never set aborts inside
       core/realm.c's `realm_value_get` ONE FRAME EARLIER — so what a worker realm really took was a shared
       helper's abort naming no site, and the clause pointed its reader at a component the crash never named.
       The lesson is the one that generalises: an absence-shows clause about a CRASH is a claim about a call
       CHAIN, so it is checked by reading what the condition's operand is computed by.
       THIS IS THE ALGORITHM ANSWERING AND NOT A FALLBACK BEING SELECTED: delete the thing it selects against
       and Web IDL still has to ask whether this interface is exposed in this realm. */
    if (!idl_exposed_in_realm(ctx, "Navigator")) return;

    prev = JS_GetClassProto(ctx, g_nav_class);
    DCHECK(JS_IsNull(prev), "navigator_install_realm ran twice in one realm — everything already holding the "
                            "first Navigator.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Navigator.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Navigator");
    for (i = 0; i < NAV_N; i++)
        idl_install_accessor_exposed(ctx, proto, NAV_NAME[i], js_nav_get, i, -1, NAV_EXPOSURE[i]);
    idl_install_accessor(ctx, proto, "userActivation", js_nav_user_activation, 0, -1);
    idl_install_accessor(ctx, proto, "permissions", js_nav_permissions, 0, -1);
    idl_install_method(ctx, proto, "javaEnabled", g_id_java_enabled);
    /* BEACON §2.1's `partial interface Navigator` — the OBJECT is HTML's and the MEMBER is that standard's, so
       its component installs it on the prototype this realm just built. It takes the prototype rather than
       reading the realm's Navigator back, because §2.1 declares an OPERATION on the interface: a method put on
       the instance would be an own property of `navigator`, absent from `Navigator.prototype`, and deletable —
       the four things this file's own header names as what makes an interface an interface. */
    navigator_beacon_install(ctx, proto);
    /* STORAGE §8 "API"'s `NavigatorStorage` MIXIN — the same rule, and it arrives here for a reason worth one
       extra sentence. Web IDL §3.7.3 "Interface prototype object" gives an `interface mixin` no prototype of
       its own, so the member has no object until an INCLUDER supplies one, and `Navigator includes
       NavigatorStorage;` makes THIS the object. It was installed on the NAVIGATOR ITSELF from that
       component's own intrinsic, which §3.7.6 "Attributes" makes a wrong answer rather than a narrow one:
       "Regular attributes are exposed on the interface prototype object, unless the attribute is unforgeable
       or if the interface was declared with the [Global] extended attribute" — and `storage` is neither, so
       `'storage' in Navigator.prototype` was false where a browser answers true, the descriptor was an own
       accessor on `navigator` where a browser has none, and `delete navigator.storage` succeeded.
       IT IS CALLED FROM HERE RATHER THAN REACHING FOR A PROTOTYPE OF ITS OWN, which is the whole reason that
       component no longer asks Web IDL §3.8 "Platform objects implementing interfaces" step 1 over
       `Navigator`: this install has already returned above where the answer is no, so a worker realm cannot
       reach the line below and there is no state left for a gate to look for. */
    storage_manager_install_navigator_storage(ctx, proto);
    idl_members_excluded(ctx, proto, "Navigator", NAV_MODE_EXCLUDED,
                         (int)(sizeof NAV_MODE_EXCLUDED / sizeof NAV_MODE_EXCLUDED[0]),
                         "HTML §8.10.1.1: the user agent supports this partial interface only if the "
                         "navigator compatibility mode is Gecko, and this one is Chrome — productSub is "
                         "\"20030107\" and vendor is \"Google Inc.\"");
    JS_SetClassProto(ctx, g_nav_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. Navigator declares no constructor, so
       `new Navigator()` is a TypeError — and its PRESENCE is what `navigator instanceof Navigator` and every
       prototype-patching polyfill needs, which is exactly what this interface had none of. */
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "Navigator", idl_interface_object(ctx, "Navigator", proto));

    nav = JS_NewObjectProtoClass(ctx, proto, g_nav_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(nav), "the Window's associated Navigator could not be allocated");
    /* THE RECORD, ATTACHED BEFORE THE OBJECT LEAVES THIS FUNCTION — which is what nav_environment's "a branded
       object without one came from a second mint that does not exist" rests on, and there is no second mint.
       The values are built HERE rather than on first read because a value minted lazily is built inside
       whichever FLOW got there first, making that flow's baseline everyone's. */
    n = calloc(1, sizeof *n);
    CHECK(n != NULL, "this realm's Navigator record could not be allocated");
    n->realm  = ctx;
    n->global = JS_DupValue(ctx, global);
    n->vals   = nav_build_values(ctx);
    JS_SetOpaque(nav, n);
    realm_value_set(ctx, g_obj_slot, nav);

    idl_install_accessor(ctx, global, "navigator", js_win_navigator, 0, -1);
    idl_install_replaceable(ctx, global, "clientInformation", js_win_navigator, 0);
    JS_FreeValue(ctx, global);
}

void navigator_init(JSContext *ctx)
{
    JSClassDef d = { "Navigator", .finalizer = nav_finalizer, .gc_mark = nav_gc_mark };

    DCHECK(g_obj_slot == JS_INVALID_CLASS_ID, "navigator_init ran twice — the class and the slot are declared once per AGENT");
    /* THE CLASS IS BOTH THE PER-REALM PROTOTYPE SLOT AND THE BRAND: the one object per realm WEARS it, so
       §3.7.6/§3.7.7's check is a class-id comparison and a page cannot forge one. */
    JS_NewClassID(JS_GetRuntime(ctx), &g_nav_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_nav_class, &d) == 0,
          "Navigator: the per-realm prototype slot could not be declared");
    g_obj_slot  = realm_value_declare(ctx, "HTML §8.10.1 the Window's associated Navigator");
    /* DECLARED once per agent and INSTALLED per realm, like every other member: a declaration builds a pool
       entry and a member has ONE, so declaring inside the install would mint a second entry for the second
       realm's prototype — which is what the pool's seal asserts against. */
    g_id_java_enabled = idl_method_id(ctx, NULL, 0, js_nav_java_enabled, 0);
    agent_state_realm_slot("navigator", &g_obj_slot,
                           "HTML §8.10.1's associated-Navigator realm slot, and the declaration latch");
    agent_state_id("navigator", &g_id_java_enabled, "§8.10.1's javaEnabled declaration");
    /* BEACON §2.1's member, declared HERE for the reason Permissions §6's whole component is declared below:
       a host that has a Navigator has `navigator.sendBeacon`, so a per-host line would be exactly the
       hand-copied list core/realm.h and core/platform.h exist to abolish. It declares no realm intrinsic of
       its own — navigator_install_realm installs it — so it is stated before that declaration rather than
       after it. */
    navigator_beacon_init(ctx);
    realm_declare_intrinsic(navigator_install_realm);
    /* PERMISSIONS §6.1 IS A PARTIAL INTERFACE OF THIS ONE, so this is where its whole component is declared —
       §3's model, §6.3's PermissionStatus and §6.2's Permissions. Declared AFTER the line above so the
       realm-intrinsic order builds this realm's Navigator before anything that reaches for it, and declared
       HERE rather than in each host's list because a host that has a Navigator has `navigator.permissions`:
       a per-host line is the hand-copied list core/realm.h exists to abolish. */
    permissions_init(ctx);
}

void navigator_free(void)
{
    /* The prototypes, the interface objects, the Navigators and their records are the REALMS' — each is
       released with its context, and a Navigator's record goes with it through nav_finalizer. What the agent
       holds is the associated-Navigator slot and the member's pool id, and a slot id is a class id in a runtime
       that is going away with it. (It read "the two slots": the member-values slot moved onto the instance, so
       there is one.) */
    g_obj_slot = JS_INVALID_CLASS_ID;
    g_id_java_enabled = -1;
    /* BEACON §2.1's member is declared from navigator_init, so it is released from here — the same rule the
       line below states for Permissions §6, and the same failure if it is not on this list. */
    navigator_beacon_free();
    /* PERMISSIONS §6's component is declared from navigator_init, so it is released from here — a component
       released from a list its declaration is not on is a component some host frees and another leaks. */
    permissions_free();
}
